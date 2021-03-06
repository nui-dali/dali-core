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

// INTERNAL INCLUDES
#include <dali/devel-api/common/stage-devel.h>
#include <dali/internal/event/common/stage-impl.h>

namespace Dali
{

namespace DevelStage
{

KeyEventGeneratedSignalType& KeyEventGeneratedSignal( Dali::Stage stage )
{
  return GetImplementation( stage ).KeyEventGeneratedSignal();
}

void SetRenderingBehavior( Dali::Stage stage, Rendering renderingBehavior )
{
  GetImplementation( stage ).SetRenderingBehavior( renderingBehavior );
}

Rendering GetRenderingBehavior( Dali::Stage stage )
{
  return GetImplementation( stage ).GetRenderingBehavior();
}

void AddFrameCallback( Dali::Stage stage, FrameCallbackInterface& frameCallback, Actor rootActor )
{
  GetImplementation( stage ).AddFrameCallback( frameCallback, GetImplementation( rootActor ) );
}

void RemoveFrameCallback( Dali::Stage stage, FrameCallbackInterface& frameCallback )
{
  GetImplementation( stage ).RemoveFrameCallback( frameCallback );
}

} // namespace DevelStage

} // namespace Dali
