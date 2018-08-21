/*
 * Copyright (c) 2018 Samsung Electronics Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <dali/graphics/vulkan/api/vulkan-api-controller.h>

#include <dali/graphics/vulkan/vulkan-graphics.h>
#include <dali/graphics/vulkan/internal/vulkan-gpu-memory-allocator.h>
#include <dali/graphics/vulkan/internal/vulkan-gpu-memory-manager.h>
#include <dali/graphics/vulkan/internal/vulkan-buffer.h>
#include <dali/graphics/vulkan/internal/vulkan-command-buffer.h>
#include <dali/graphics/vulkan/internal/vulkan-command-pool.h>
#include <dali/graphics/vulkan/internal/vulkan-descriptor-set.h>
#include <dali/graphics/vulkan/internal/vulkan-pipeline.h>
#include <dali/graphics/vulkan/internal/vulkan-framebuffer.h>
#include <dali/graphics/vulkan/internal/vulkan-surface.h>
#include <dali/graphics/vulkan/internal/vulkan-sampler.h>
#include <dali/graphics/vulkan/internal/vulkan-image.h>
#include <dali/graphics/vulkan/internal/vulkan-swapchain.h>
#include <dali/graphics/vulkan/internal/vulkan-debug.h>
#include <dali/graphics/vulkan/internal/vulkan-fence.h>

// API
#include <dali/graphics/vulkan/api/vulkan-api-shader.h>
#include <dali/graphics/vulkan/api/vulkan-api-texture.h>
#include <dali/graphics/vulkan/api/vulkan-api-buffer.h>
#include <dali/graphics/vulkan/api/vulkan-api-framebuffer.h>
#include <dali/graphics/vulkan/api/vulkan-api-texture-factory.h>
#include <dali/graphics/vulkan/api/vulkan-api-shader-factory.h>
#include <dali/graphics/vulkan/api/vulkan-api-buffer-factory.h>
#include <dali/graphics/vulkan/api/vulkan-api-pipeline.h>
#include <dali/graphics/vulkan/api/vulkan-api-pipeline-factory.h>
#include <dali/graphics/vulkan/api/vulkan-api-framebuffer-factory.h>
#include <dali/graphics/vulkan/api/vulkan-api-sampler-factory.h>
#include <dali/graphics/vulkan/api/vulkan-api-render-command.h>
#include <dali/graphics/vulkan/api/internal/vulkan-pipeline-cache.h>
#include <dali/graphics/vulkan/api/internal/vulkan-ubo-manager.h>

namespace Dali
{
namespace Graphics
{
namespace VulkanAPI
{
namespace
{
constexpr uint32_t MAX_SEC_BUFFER_ARRAYS = 3u;
}

struct Controller::Impl
{
  struct RenderPassChange
  {
    // only move semantics
    RenderPassChange( vk::RenderPassBeginInfo _beginInfo,
                      std::vector< vk::ClearValue >&& _clearColorValues,
                      const Vulkan::RefCountedFramebuffer& _framebuffer,
                      uint32_t _offset )
            : beginInfo( std::move( _beginInfo ) ),
              colorValues( std::move( _clearColorValues ) ),
              framebuffer( _framebuffer ),
              offset( _offset )
    {
    }

    // no default constructor!
    RenderPassChange() = delete;

    vk::RenderPassBeginInfo beginInfo{};
    std::vector< vk::ClearValue > colorValues{};
    Vulkan::RefCountedFramebuffer framebuffer{};
    uint32_t offset{ 0 };
  };

  Impl( Controller& owner, Dali::Graphics::Vulkan::Graphics& graphics )
          : mGraphics( graphics ),
            mOwner( owner ),
            mBufferRefsIndex(0u)
  {
  }

  ~Impl() = default;

  // TODO: @todo this function initialises basic buffers, shaders and pipeline
  // for the prototype ONLY
  bool Initialise()
  {
    // Create factories
    mShaderFactory = MakeUnique< VulkanAPI::ShaderFactory >( mGraphics );
    mTextureFactory = MakeUnique< VulkanAPI::TextureFactory >( mOwner );
    mBufferFactory = MakeUnique< VulkanAPI::BufferFactory >( mOwner );
    mFramebufferFactory = MakeUnique< VulkanAPI::FramebufferFactory >( mOwner );
    mPipelineFactory = MakeUnique< VulkanAPI::PipelineFactory >( mOwner );
    mSamplerFactory = MakeUnique< VulkanAPI::SamplerFactory >( mOwner );

    mUboManager = MakeUnique< VulkanAPI::UboManager >( mOwner );

    mDefaultPipelineCache = MakeUnique< VulkanAPI::PipelineCache >( mGraphics, mOwner );

    return true;
  }

  void BeginFrame()
  {
    mSecondaryCommandBufferRefs[mBufferRefsIndex].clear();

    // for all swapchains acquire new framebuffer
    auto surface = mGraphics.GetSurface( 0u );

    auto swapchain = mGraphics.GetSwapchainForFBID( 0u );

    swapchain->AcquireNextFramebuffer();

    if( !swapchain->IsValid() )
    {
      // make sure device doesn't do any work before replacing swapchain
      mGraphics.DeviceWaitIdle();

      // replace swapchain
      swapchain = mGraphics.ReplaceSwapchainForSurface( surface, std::move(swapchain) );

      // get new valid framebuffer
      swapchain->AcquireNextFramebuffer();
    }

    mCurrentFramebuffer.Reset();
  }

  void EndFrame()
  {
    // swap all swapchains
    auto swapchain = mGraphics.GetSwapchainForFBID( 0u );

    auto primaryCommandBuffer = swapchain->GetCurrentCommandBuffer();

    for( auto i = 0u; i < mRenderPasses.size(); ++i )
    {
      if( i != 0u )
      {
        primaryCommandBuffer->EndRenderPass();
      }

      const auto& rp = mRenderPasses[i];
      uint32_t offset = rp.offset;
      auto count = uint32_t(
              ( i == mRenderPasses.size() - 1 ) ?
              mSecondaryCommandBufferRefs[mBufferRefsIndex].size() - rp.offset :
              mRenderPasses[i + 1].offset - rp.offset );

      primaryCommandBuffer->BeginRenderPass( rp.beginInfo, vk::SubpassContents::eSecondaryCommandBuffers );
      primaryCommandBuffer->ExecuteCommands( mSecondaryCommandBufferRefs[mBufferRefsIndex], offset, count );
    }

    if( !mRenderPasses.empty() )
    {
      primaryCommandBuffer->EndRenderPass();
    }
    else
    {
      primaryCommandBuffer->BeginRenderPass( vk::RenderPassBeginInfo{}
        .setFramebuffer( swapchain->GetCurrentFramebuffer()->GetVkHandle() )
        .setRenderPass(swapchain->GetCurrentFramebuffer()->GetRenderPass() )
        .setRenderArea( { {0, 0}, { swapchain->GetCurrentFramebuffer()->GetWidth(), swapchain->GetCurrentFramebuffer()->GetHeight() } } )
        .setPClearValues( swapchain->GetCurrentFramebuffer()->GetClearValues().data() )
        .setClearValueCount( uint32_t(swapchain->GetCurrentFramebuffer()->GetClearValues().size()) ), vk::SubpassContents::eSecondaryCommandBuffers );
      primaryCommandBuffer->EndRenderPass();
    }

    swapchain->Present();
    mRenderPasses.clear();

    if( !swapchain->IsValid() )
    {
      // get surface associated with 'main' framebuffer
      auto surface = mGraphics.GetSurface( 0u );

      // wait till device is idle, no more GPU work done
      mGraphics.DeviceWaitIdle();

      // replace swapchain
      mGraphics.ReplaceSwapchainForSurface( surface, std::move(swapchain) );
    }

    ++mBufferRefsIndex;

    mBufferRefsIndex %= MAX_SEC_BUFFER_ARRAYS;
  }

  API::TextureFactory& GetTextureFactory() const
  {
    return *( mTextureFactory.get() );
  }

  API::ShaderFactory& GetShaderFactory() const
  {
    return *( mShaderFactory.get() );
  }

  API::BufferFactory& GetBufferFactory() const
  {
    return *( mBufferFactory.get() );
  }

  API::FramebufferFactory& GetFramebufferFactory() const
  {
    mFramebufferFactory->Reset();
    return *( mFramebufferFactory.get() );
  }


  std::unique_ptr< API::RenderCommand > AllocateRenderCommand()
  {
    return std::make_unique< VulkanAPI::RenderCommand >( mOwner, mGraphics );
  }

  bool UpdateRenderPass( const API::RenderCommand::RenderTargetBinding& renderTargetBinding, uint32_t offset )
  {
    Vulkan::RefCountedFramebuffer framebuffer{ nullptr };
    if( renderTargetBinding.framebuffer )
    {
      framebuffer = static_cast<const VulkanAPI::Framebuffer&>(*renderTargetBinding.framebuffer).GetFramebufferRef();
    }
    else
    {
      // use first surface/swapchain as render target
      auto surface = mGraphics.GetSurface( 0u );
      auto swapchain = mGraphics.GetSwapchainForFBID( 0u );
      framebuffer = swapchain->GetCurrentFramebuffer();
    }

    if( framebuffer != mCurrentFramebuffer )
    {
      mCurrentFramebuffer = framebuffer;

      // @todo Add Pipeline Barrier



      // @todo Only do if there is a color attachment and color clear is enabled and there is a clear color
      auto newColors = mCurrentFramebuffer->GetClearValues();
      newColors[0].color.setFloat32( { renderTargetBinding.clearColors[0].r,
                                       renderTargetBinding.clearColors[0].g,
                                       renderTargetBinding.clearColors[0].b,
                                       renderTargetBinding.clearColors[0].a
                                     } );
      if( 0 ) // @todo Only do if there is a depth buffer & depth clear is enabled
      {
        newColors[1].setDepthStencil( vk::ClearDepthStencilValue{}
                                      .setDepth( 0.0f )
                                      .setStencil( 0 ));
      }

      mRenderPasses.emplace_back(
              vk::RenderPassBeginInfo{}
                      .setRenderPass( mCurrentFramebuffer->GetRenderPass() )
                      .setFramebuffer( mCurrentFramebuffer->GetVkHandle() )
                      .setRenderArea( vk::Rect2D( { 0, 0 }, { mCurrentFramebuffer->GetWidth(),
                                                              mCurrentFramebuffer->GetHeight() } ) )
                      .setClearValueCount( uint32_t( newColors.size() ) )
                      .setPClearValues( newColors.data() ),
              std::move( newColors ),
              framebuffer,
              offset );
      return true;
    }

    // same render pass
    return false;
  }

  /**
   * Submits number of commands in one go ( simiar to vkCmdExecuteCommands )
   * @param commands
   */
  void SubmitCommands( std::vector< Dali::Graphics::API::RenderCommand* > commands )
  {
    // if there are any scheduled writes
    if( !mBufferTransferRequests.empty() )
    {
      for( auto&& req : mBufferTransferRequests )
      {
        void* dst = req->dstBuffer->GetMemoryHandle()->Map();
        memcpy( dst, &*req->srcPtr, req->srcSize );
        req->dstBuffer->GetMemoryHandle()->Unmap();
      }
      mBufferTransferRequests.clear();
    }

    // execute all scheduled resource transfers
    ProcessResourceTransferRequests();

    // the list of commands may be empty, but still we may have scheduled memory
    // transfers
    if( commands.empty() )
    {
      return;
    }

    // bind resources
    for( auto&& command : commands )
    {
      auto apiCommand = static_cast<VulkanAPI::RenderCommand*>(command);
      apiCommand->PrepareResources();
    }

    // Update uniform buffers
    for( auto&& command : commands )
    {
      // prepare pipelines
      auto apiCommand = static_cast<VulkanAPI::RenderCommand*>(command);
      apiCommand->UpdateUniformBuffers();
    }

    mUboManager->UnmapAllBuffers();

    // Begin render pass for render target
    // clear color obtained from very first command in the batch
    auto firstCommand = static_cast<VulkanAPI::RenderCommand*>(commands[0]);
    UpdateRenderPass( firstCommand->GetRenderTargetBinding(), Vulkan::U32(mSecondaryCommandBufferRefs[mBufferRefsIndex].size()) );

    // set up writes
    for( auto&& command : commands )
    {
#if defined(DEBUG_ENABLED)
      if( getenv( "LOG_VULKAN_API" ) )
      {
        DALI_LOG_STREAM( gVulkanFilter, Debug::General, *command );
      }
#endif

      auto apiCommand = static_cast<VulkanAPI::RenderCommand*>(command);

      // skip if there's no valid pipeline
      if( !apiCommand->GetVulkanPipeline() )
      {
        continue;
      }
      auto inheritanceInfo = vk::CommandBufferInheritanceInfo{}
         .setRenderPass( mCurrentFramebuffer->GetRenderPass() )
         .setFramebuffer( mCurrentFramebuffer->GetVkHandle() );

      // start new command buffer
      auto cmdbuf = mGraphics.CreateCommandBuffer( false );
      cmdbuf->Begin( vk::CommandBufferUsageFlagBits::eRenderPassContinue, &inheritanceInfo );
      cmdbuf->BindGraphicsPipeline( apiCommand->GetVulkanPipeline() );
      //@todo add assert to check the pipeline render pass nad the inherited render pass are the same

      // set dynamic state
      if( apiCommand->mDrawCommand.scissorTestEnable )
      {
        vk::Rect2D scissorRect( { apiCommand->mDrawCommand.scissor.x,
                                  apiCommand->mDrawCommand.scissor.y },
                                { apiCommand->mDrawCommand.scissor.width,
                                  apiCommand->mDrawCommand.scissor.height } );

        cmdbuf->SetScissor( 0, 1, &scissorRect );
      }

      // dynamic state: viewport
      auto vulkanApiPipeline = static_cast<const VulkanAPI::Pipeline*>(apiCommand->GetPipeline());
      auto dynamicStateMask = vulkanApiPipeline->GetDynamicStateMask();
      if( (dynamicStateMask & API::PipelineDynamicStateBits::VIEWPORT_BIT) && apiCommand->mDrawCommand.viewportEnable )
      {
        auto viewportRect = apiCommand->mDrawCommand.viewport;

        vk::Viewport viewport( viewportRect.x,
                               viewportRect.y,
                               viewportRect.width,
                               viewportRect.height,
                               viewportRect.minDepth,
                               viewportRect.maxDepth );

        cmdbuf->SetViewport( 0, 1, &viewport );
      }

      // bind vertex buffers
      auto binding = 0u;
      for( auto&& vb : apiCommand->GetVertexBufferBindings() )
      {
        cmdbuf->BindVertexBuffer( binding++, static_cast<const VulkanAPI::Buffer&>( *vb ).GetBufferRef(), 0 );
      }

      // note: starting set = 0
      cmdbuf->BindDescriptorSets( apiCommand->GetDescriptorSets(), 0 );

      // draw
      const auto& drawCommand = apiCommand->GetDrawCommand();

      const auto& indexBinding = apiCommand->GetIndexBufferBinding();
      if( indexBinding.buffer )
      {
        cmdbuf->BindIndexBuffer( static_cast<const VulkanAPI::Buffer&>(*indexBinding.buffer).GetBufferRef(),
                                 0, vk::IndexType::eUint16 );
        cmdbuf->DrawIndexed( drawCommand.indicesCount,
                             drawCommand.instanceCount,
                             drawCommand.firstIndex,
                             0,
                             drawCommand.firstInstance );
      }
      else
      {
        cmdbuf->Draw( drawCommand.vertexCount,
                      drawCommand.instanceCount,
                      drawCommand.firstVertex,
                      drawCommand.firstInstance );
      }
      cmdbuf->End();
      mSecondaryCommandBufferRefs[mBufferRefsIndex].emplace_back( cmdbuf );
    }
  }

  void ProcessResourceTransferRequests( bool immediateOnly = false )
  {
    if(!mResourceTransferRequests.empty())
    {
      // for images we need to generate all the barriers to make sure the layouts
      // are correct.
      std::vector<vk::ImageMemoryBarrier> preLayoutBarriers;
      std::vector<vk::ImageMemoryBarrier> postLayoutBarriers;

      auto layout = vk::ImageMemoryBarrier{}
        .setSubresourceRange( vk::ImageSubresourceRange{}
                                .setLayerCount( 1 )
                                .setBaseArrayLayer( 0 )
                                .setAspectMask( vk::ImageAspectFlagBits::eColor )
                                .setBaseMipLevel( 0 )
                                .setLevelCount( 1 ))
        .setImage( nullptr )
        .setOldLayout( {} )
        .setNewLayout( vk::ImageLayout::eTransferDstOptimal )
        .setSrcAccessMask( vk::AccessFlagBits::eMemoryRead )
        .setDstAccessMask( vk::AccessFlagBits::eMemoryWrite );

      // Create barriers for unique images
      for(const auto& req : mResourceTransferRequests)
      {
        if( !immediateOnly || !req.deferredTransferMode )
        {
          if( req.requestType == TransferRequestType::BUFFER_TO_IMAGE )
          {
            // add barrier
            auto image = req.bufferToImageInfo.dstImage;
            preLayoutBarriers.push_back( mGraphics.CreateImageMemoryBarrier( image, image->GetImageLayout(), vk::ImageLayout::eTransferDstOptimal ) );
            postLayoutBarriers.push_back( mGraphics.CreateImageMemoryBarrier( image, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eShaderReadOnlyOptimal ) );
            image->SetImageLayout( vk::ImageLayout::eShaderReadOnlyOptimal );
          }
          else if( req.requestType == TransferRequestType::IMAGE_TO_IMAGE )
          {
            auto dstImage = req.imageToImageInfo.dstImage;
            auto srcImage = req.imageToImageInfo.srcImage;
            preLayoutBarriers.push_back( layout.setImage( dstImage->GetVkHandle() )
                                               .setOldLayout( dstImage->GetImageLayout() )
                                               .setNewLayout( vk::ImageLayout::eTransferDstOptimal ));
            postLayoutBarriers.push_back( layout.setImage( dstImage->GetVkHandle() )
                                               .setNewLayout( dstImage->GetImageLayout() )
                                               .setOldLayout( vk::ImageLayout::eTransferDstOptimal ));

            preLayoutBarriers.push_back( layout.setImage( srcImage->GetVkHandle() )
                                               .setOldLayout( srcImage->GetImageLayout() )
                                               .setNewLayout( vk::ImageLayout::eTransferSrcOptimal ));
            postLayoutBarriers.push_back( layout.setImage( srcImage->GetVkHandle() )
                                               .setNewLayout( srcImage->GetImageLayout() )
                                               .setOldLayout( vk::ImageLayout::eTransferSrcOptimal ));
          }
        }
      }

      auto commandBuffer = mGraphics.CreateCommandBuffer( true );
      commandBuffer->Begin( vk::CommandBufferUsageFlagBits::eOneTimeSubmit, nullptr );

      // issue memory barrier
      commandBuffer->PipelineBarrier( vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, preLayoutBarriers );

      // issue blit or copy commands
      for(const auto& req : mResourceTransferRequests)
      {
        if( !immediateOnly || !req.deferredTransferMode )
        {
          if( req.requestType == TransferRequestType::BUFFER_TO_IMAGE )
          {
            commandBuffer->CopyBufferToImage( req.bufferToImageInfo.srcBuffer,
                                              req.bufferToImageInfo.dstImage, vk::ImageLayout::eTransferDstOptimal,
                                              { req.bufferToImageInfo.copyInfo } );
          }
          else if( req.requestType == TransferRequestType::IMAGE_TO_IMAGE )
          {
            commandBuffer->CopyImage( req.imageToImageInfo.srcImage,
                                      vk::ImageLayout::eTransferSrcOptimal,
                                      req.imageToImageInfo.dstImage,
                                      vk::ImageLayout::eTransferDstOptimal,
                                      { req.imageToImageInfo.copyInfo });
          }
        }
      }

      commandBuffer->PipelineBarrier( vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, postLayoutBarriers );
      commandBuffer->End();

      // submit to the queue
      auto transferQueue = mGraphics.GetTransferQueue( 0u );

      // no fence, just semaphore
      auto fence = mGraphics.CreateFence( {} );

      mGraphics.Submit( mGraphics.GetTransferQueue(0u), { Vulkan::SubmissionData{ {}, {}, { commandBuffer }, {} } }, fence );

      // @todo use semaphores rather than fences
      mGraphics.WaitForFence( fence );

      // when transferring only immediate requests we can't clear all
      if( !immediateOnly )
      {
        mResourceTransferRequests.clear();
      }
      else
      {
        decltype(mResourceTransferRequests) tmp;
        for(auto&& req : mResourceTransferRequests)
        {
          if( req.deferredTransferMode )
          {
            tmp.emplace_back( std::move( req ) );
          }
        }
        mResourceTransferRequests = std::move(tmp);
      }
    }

  }

  std::unique_ptr< PipelineCache > mDefaultPipelineCache;

  Vulkan::Graphics& mGraphics;
  Controller& mOwner;
  uint32_t mBufferRefsIndex;

  std::unique_ptr< VulkanAPI::TextureFactory > mTextureFactory;
  std::unique_ptr< VulkanAPI::ShaderFactory > mShaderFactory;
  std::unique_ptr< VulkanAPI::BufferFactory > mBufferFactory;
  std::unique_ptr< VulkanAPI::PipelineFactory > mPipelineFactory;
  std::unique_ptr< VulkanAPI::FramebufferFactory > mFramebufferFactory;
  std::unique_ptr< VulkanAPI::SamplerFactory > mSamplerFactory;

  // used for UBOs
  std::vector< std::unique_ptr< VulkanAPI::BufferMemoryTransfer > > mBufferTransferRequests;

  // used for texture<->buffer<->memory transfers
  std::vector< ResourceTransferRequest >                            mResourceTransferRequests;

  std::unique_ptr< VulkanAPI::UboManager > mUboManager;

  // Accumulate all the secondary command buffers of the frame here to avoid them being overwritten
  // This accumulator vector gets cleared at the end of the frame. The command buffers are returned to the pool
  // and ready to be used for the next frame.
  std::vector< Vulkan::RefCountedCommandBuffer > mSecondaryCommandBufferRefs[MAX_SEC_BUFFER_ARRAYS];

  Vulkan::RefCountedFramebuffer mCurrentFramebuffer;
  std::vector< RenderPassChange > mRenderPasses;

};

// TODO: @todo temporarily ignore missing return type, will be fixed later

std::unique_ptr< API::Shader > Controller::CreateShader( const API::BaseFactory< API::Shader >& factory )
{
  return factory.Create();
}

std::unique_ptr< API::Texture > Controller::CreateTexture( const API::BaseFactory< API::Texture >& factory )
{
  return factory.Create();
}

std::unique_ptr< API::Buffer > Controller::CreateBuffer( const API::BaseFactory< API::Buffer >& factory )
{
  return factory.Create();
}

std::unique_ptr< API::Sampler > Controller::CreateSampler( const API::BaseFactory< API::Sampler >& factory )
{
  return factory.Create();
}

std::unique_ptr< API::Pipeline > Controller::CreatePipeline( const API::BaseFactory< API::Pipeline >& factory )
{
  auto& pipelineFactory = const_cast<PipelineFactory&>(dynamic_cast<const PipelineFactory&>( factory ));

  // if no custom cache, use default one
  if( !pipelineFactory.mPipelineCache )
  {
    pipelineFactory.mPipelineCache = mImpl->mDefaultPipelineCache.get();
  }

  return mImpl->mPipelineFactory->Create();
}

std::unique_ptr< API::Framebuffer > Controller::CreateFramebuffer( const API::BaseFactory< API::Framebuffer >& factory )
{
  return factory.Create();
}

std::unique_ptr< Controller > Controller::New( Vulkan::Graphics& vulkanGraphics )
{
  return std::make_unique< Controller >( vulkanGraphics );
}

Controller::Controller( Vulkan::Graphics& vulkanGraphics ) : mImpl( std::make_unique< Impl >( *this, vulkanGraphics ) )
{
}

void Controller::Initialise()
{
  mImpl->Initialise();
}

Controller::~Controller() = default;

Controller::Controller() = default;

Controller& Controller::operator=( Controller&& ) noexcept = default;

void Controller::BeginFrame()
{
  mImpl->BeginFrame();
}

void Controller::EndFrame()
{
  mImpl->EndFrame();
}

API::TextureFactory& Controller::GetTextureFactory() const
{
  return mImpl->GetTextureFactory();
}

API::ShaderFactory& Controller::GetShaderFactory() const
{
  return mImpl->GetShaderFactory();
}

API::BufferFactory& Controller::GetBufferFactory() const
{
  return mImpl->GetBufferFactory();
}

API::FramebufferFactory& Controller::GetFramebufferFactory() const
{
  return mImpl->GetFramebufferFactory();
}

API::PipelineFactory& Controller::GetPipelineFactory()
{
  mImpl->mPipelineFactory->Reset();
  return *mImpl->mPipelineFactory;
}

API::SamplerFactory& Controller::GetSamplerFactory()
{
  return mImpl->mSamplerFactory->Reset();
}

Vulkan::Graphics& Controller::GetGraphics() const
{
  return mImpl->mGraphics;
}

void Controller::ScheduleBufferMemoryTransfer( std::unique_ptr< VulkanAPI::BufferMemoryTransfer > transferRequest )
{
  mImpl->mBufferTransferRequests.emplace_back( std::move( transferRequest ) );
}

void Controller::ScheduleResourceTransfer( VulkanAPI::ResourceTransferRequest&& transferRequest )
{
  mImpl->mResourceTransferRequests.emplace_back( std::move(transferRequest) );

  // if we requested immediate upload then request will be processed instantly with skipping
  // all the deferred update requests
  if( !mImpl->mResourceTransferRequests.back().deferredTransferMode )
  {
    mImpl->ProcessResourceTransferRequests( true );
  }
}

VulkanAPI::UboManager& Controller::GetUboManager()
{
  return *mImpl->mUboManager;
}

void Controller::SubmitCommands( std::vector< API::RenderCommand* > commands )
{
  mImpl->SubmitCommands( std::move( commands ) );
}

std::unique_ptr< API::RenderCommand > Controller::AllocateRenderCommand()
{
  return mImpl->AllocateRenderCommand();
}

} // namespace VulkanAPI
} // namespace Graphics
} // namespace Dali