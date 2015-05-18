#ifndef __DALI_TYPE_TRAITS_H__
#define __DALI_TYPE_TRAITS_H__

/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
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

namespace Dali
{

/**
 * @brief Basic type traits that every type has by default
 * This allows specialisations to not have to repeat all flags
 */
template <typename Type>
struct BasicTypes
{
  /**
   * This flag tells Dali if a class can be considered POD. If it declares copy constructor and/or destructor, its not considered trivial
   * and cannot be copied by using memcpy etc.
   */
  enum { IS_TRIVIAL_TYPE = __has_trivial_destructor(Type) && __has_trivial_copy(Type) };
};

/**
 * @brief Type traits support
 * An example of overriding a traits flag for a custom type can be done by:
 * <code>
 * namespace Dali
 * {
 *   /// Tell DALi that Matrix is POD, even though it has a copy constructor
 *   template <> struct TypeTraits< Matrix > : public BasicTypes< Matrix > { enum { IS_TRIVIAL_TYPE = true }; };
 * }
 * </code>
 */
template <typename Type>
struct TypeTraits : public BasicTypes< Type >
{
};

} // namespace Dali

#endif /* __DALI_TYPE_TRAITS_H__ */