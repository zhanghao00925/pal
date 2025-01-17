/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2021 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
***********************************************************************************************************************
* @file  palStringBag.h
* @brief PAL utility collection StringBag and StringBagIterator class declarations.
***********************************************************************************************************************
*/

#pragma once

#include "palUtil.h"
#include "palAssert.h"
#include "palSysMemory.h"
#include <type_traits>

namespace Util
{

// Forward declarations.
template<typename T, typename Allocator> class StringBag;

// Type aliases.
using StringBagHandle = uint32;

/**
***********************************************************************************************************************
* @brief  Iterator for traversal of strings in StringBag.
*
* Supports forward traversal.
***********************************************************************************************************************
*/
template<typename T, typename Allocator>
class StringBagIterator
{
public:
    ~StringBagIterator() {}

    /// Checks if the current index is within bounds of the strings in the bag.
    ///
    /// @returns True if the current string this iterator is pointing to is within the permitted range.
    bool IsValid() const { return (m_currIndex < m_pSrcBag->m_currOffset); }

    /// Returns the string the iterator is currently pointing to as const pointer.
    ///
    /// @warning This may cause an access violation if the iterator is not valid.
    ///
    /// @returns The string the iterator is currently pointing to as const pointer.
    const T* Get() const
    {
        PAL_ASSERT(IsValid());
        return (m_pSrcBag->m_pData + m_currIndex);
    }

    /// Returns a handle for the string the iterator is currently pointing to.
    ///
    /// @warning This may cause an access violation if the iterator is not valid.
    ///
    /// @returns A handle for the string the iterator is currently pointing to.
    StringBagHandle GetHandle() const
    {
        PAL_ASSERT(IsValid());
        return Position();
    }

    /// Advances the iterator to the next string.  Iterating to the next string in the container is an O(N) operation,
    /// where N is the length of the current string.
    ///
    /// @warning Does not do bounds checking.
    void Next()
    {
        PAL_ASSERT(IsValid());
        do
        {
            m_currIndex++;
            PAL_ASSERT(m_currIndex <= m_pSrcBag->m_maxCapacity);
        } while (m_pSrcBag->m_pData[m_currIndex - 1] != T(0));
    }

    /// Retrieves the current position of this iterator.
    ///
    /// @returns The location in the bag the iterator is currently pointing to.
    uint32 Position() const { return m_currIndex; }

private:
    StringBagIterator(uint32 index, const StringBag<T, Allocator>* pSrcBag);

    uint32                          m_currIndex; // The current index of the bag iterator.
    const StringBag<T, Allocator>*  m_pSrcBag;   // The bag container this iterator is used for.

    StringBagIterator() = delete; // Default constructor.

    // Although this is a transgression of coding standards, it means that StringBag does not need to have a public
    // interface specifically to implement this class. The added encapsulation this provides is worthwhile.
    friend class StringBag<T, Allocator>;
};

/**
***********************************************************************************************************************
* @brief StringBag container.
*
* StringBag is a templated array based storage. If space is needed it dynamically allocates double the required
* size every time the capacity is exceeded. Operations which this class supports are:
*
* - Insertion at the end of the array.
* - Forward iteration.
* - Random access from valid handles.
*
* @warning This class is not thread-safe.
***********************************************************************************************************************
*/
template<typename T, typename Allocator>
class StringBag
{
    static_assert((std::is_same<T, char>::value || std::is_same<T, wchar_t>::value),
                  "StringBag type T must be either char or wchar_t.");
public:
    /// A convenient shorthand for StringBagIterator.
    using Iter = StringBagIterator<T, Allocator>;

    /// Constructor.
    ///
    /// @param [in] pAllocator The allocator that will allocate memory if required.
    explicit StringBag(Allocator*const pAllocator);

    /// Destructor.
    ~StringBag();

    /// Increases maximal buffer capacity to a value greater or equal to the @ref newCapacity.
    /// If @ref newCapacity is greater than the maximal buffer capacity, new storage is allocated,
    /// otherwise the method does nothing.
    ///
    /// @note All existing iterators, and handles will not get invalidated, even in case new storage is allocated,
    ///       because iterators are referencing the bag, rather than memory of that bag.
    ///
    /// @warning All pointers and references to strings of a bag will be invalidated,
    ///          in the case new storage is allocated.
    ///
    /// @param [in] newCapacity The new capacity of the bag, which is lower limit of the maximal capacity. The
    ///                         units are in terms of characters (T).
    ///
    /// @returns Result ErrorOutOfMemory if the operation failed.
    Result Reserve(uint32 newCapacity)
    {
        Result result = Result::_Success;
        // Not enough storage.
        if (m_maxCapacity < newCapacity)
        {
            result = ReserveInternal(newCapacity);
        }
        return result;
    }

    /// Copy a string to end of the bag. If there is not enough space available, new space will be allocated
    /// and the old strings will be copied to the new space.
    ///
    /// @warning Calling with a @ref pString that doesn't have a null terminator results in undefined behavior.
    ///
    /// @warning Calling with an invalid @ref pResult pointer will cause an access violation!
    ///
    /// @param [in] pString  The string to be pushed to the bag. The string will become the last string in the
    ///                      bag.
    ///
    /// @param [out] pResult - Set to ErrorInvalidPointer if @ref pString an invalid pointer, but leaves the bag
    ///                        in an unmodified state.
    ///                      - Set to ErrorOutOfMemory if the operation failed because memory couldn't be allocated.
    ///                      - Set to ErrorInvalidMemorySize if the operation would result in a memory allocation
    ///                        larger than the maximum value possible for a uint32.
    ///
    /// @returns A valid handle to the inserted string if @ref pResult is set to Success. The handle is the
    ///          interger offset to the start of the inserted string in the bag.
    StringBagHandle PushBack(const T* pString, Result* pResult);

    /// Copy a string to end of the bag. If there is not enough space available, new space will be allocated
    /// and the old strings will be copied to the new space.
    ///
    /// @note If the length the string must be calculated, or is already known before insertion this overload of
    ///       PushBack() should be a slight optimization.
    ///
    /// @warning Calling with an invalid @ref length results in undefined behavior!
    ///
    /// @warning Calling with an invalid @ref pResult pointer will cause an access violation!
    ///
    /// @param [in] pString  The string to be pushed to the bag. The string will become the last string in the
    ///                      bag.
    ///
    /// @param [in] length   The length of the string to be copied not including the null terminator.
    ///
    /// @param [out] pResult - Set to ErrorInvalidPointer if @ref pString an invalid pointer, but leaves the bag
    ///                        in an unmodified state.
    ///                      - Set to ErrorOutOfMemory if the operation failed because memory couldn't be allocated.
    ///                      - Set to ErrorInvalidMemorySize if the operation would result in a memory allocation
    ///                        larger than the maximum value possible for a uint32.
    ///
    /// @returns A valid handle to the inserted string if @ref pResult is set to Success. The handle is the
    ///          interger offset to the start of the inserted string in the bag.
    StringBagHandle PushBack(const T* pString, uint32 length, Result* pResult);

    /// Resets the bag. All dynamically allocated memory will be saved for reuse.
    ///
    /// @note All existing iterators, handles, and pointers to internal bag data will be invalidated.
    void Clear()
    {
        m_currOffset = 0;
        m_prevOffset = 0;
    }

    /// Returns the string specified by @ref handle.
    ///
    /// @warning Calling this function with an out-of-bounds @ref handle will cause an access violation!
    ///
    /// @warning Calling this function with an invalid @ref handle results in undefined behavior!
    ///
    /// @param [in]  handle    Integer offset to the start of a string in the bag. Valid handles can
    ///                        only be obtained from:
    ///                             - @ref PushBack()
    ///                             - @ref BackHandle()
    ///                             - @ref StringBagIterator::GetHandle()
    ///
    /// @returns A const pointer to the string at the location specified by @ref handle.
    const T* At(StringBagHandle handle) const
    {
        PAL_ASSERT(handle < m_currOffset);
        return (m_pData + handle);
    }

    /// Returns the string at the back of the bag.
    ///
    /// @warning Calling this function on an empty bag will cause an access violation!
    ///
    /// @returns A const pointer to the string at the back of the bag.
    const T* Back() const
    {
        PAL_ASSERT(IsEmpty() == false);
        return (m_pData + m_prevOffset);
    }

    /// Returns a handle to the string at the back of the bag.
    ///
    /// @warning Calling this function on an empty bag will cause an access violation!
    ///
    /// @returns A handle to the string at the back of the bag.
    StringBagHandle BackHandle() const
    {
        PAL_ASSERT(IsEmpty() == false);
        return m_prevOffset;
    }

    /// Returns an iterator to the first string in the bag.
    ///
    /// @warning If the bag is empty the iterator is immediately invalid.
    ///
    /// @returns An iterator to first string in the bag.
    Iter Begin() const { return Iter(0, this); }

    /// Returns a pointer to the underlying buffer serving as the data storage.
    /// The returned pointer defines an always valid range [Data(), Data() + NumElements()),
    /// even if the container is empty (Data() is not dereferenceable in that case).
    ///
    /// @warning Dereferencing a pointer returned by Data() from an empty bag will cause an access violation!
    ///
    /// @returns Pointer to the underlying data storage for read only access.
    ///          For a non-empty bag, the returned pointer contains the address of the first string.
    ///          For an empty bag, the returned pointer may or may not be a null pointer.
    const T* Data() const { return m_pData; }

    /// Returns the size of the bag in characters.
    ///
    /// @returns An unsigned integer equal to the number of characters in all the strings currently present
    ///          in the bag. The size in bytes of this portion of the data buffer is equal to:
    ///                 size(T) * NumChars()
    uint32 NumChars() const { return m_currOffset; }

    /// Returns true if the number of strings present in the bag is equal to zero.
    ///
    /// @returns True if the bag is empty.
    bool IsEmpty() const { return (m_currOffset == 0); }

    /// Returns a pointer to the allocator used for this container's memory management.
    ///
    /// @returns Allocator pointer.
    Allocator* GetAllocator() const { return m_pAllocator; }

private:
    Result ReserveInternal(uint32 newCapacity);
    Result PushBackInternal(const T* pString, uint32 length);

    T*               m_pData;        // Pointer to the string buffer.
    uint32           m_currOffset;   // Current character offset into the string buffer.
    StringBagHandle  m_prevOffset;   // Previous character offset into the string buffer.
    uint32           m_maxCapacity;  // Maximum size it can hold.
    Allocator*const  m_pAllocator;   // Allocator for this StringBag.

    StringBag()                            = delete; // Default constructor.
    StringBag(StringBag const &)           = delete; // Copy construtor.
    StringBag& operator=(const StringBag&) = delete; // Copy assignment operator.
    StringBag(StringBag&& bag)             = delete; // Move constructor.
    StringBag& operator=(StringBag&& bag)  = delete; // Move assignment operator.

    // Although this is a transgression of coding standards, it prevents StringBagIterator requiring a public constructor;
    // constructing a 'bare' StringBagIterator (i.e. without calling StringBag::GetIterator) can never be a legal operation,
    // so this means that these two classes are much safer to use.
    friend class StringBagIterator<T, Allocator>;
};

// =====================================================================================================================
template<typename T, typename Allocator>
StringBagIterator<T, Allocator>::StringBagIterator(
    uint32                         index,
    const StringBag<T, Allocator>* pSrcBag)
    :
    m_currIndex(index),
    m_pSrcBag(pSrcBag)
{}

// =====================================================================================================================
template<typename T, typename Allocator>
StringBag<T, Allocator>::StringBag(
    Allocator*const pAllocator)
    :
    m_pData(nullptr),
    m_currOffset(0),
    m_prevOffset(0),
    m_maxCapacity(0),
    m_pAllocator(pAllocator)
{}

// =====================================================================================================================
template<typename T, typename Allocator>
StringBag<T, Allocator>::~StringBag()
{
    // Check if we have dynamically allocated memory.
    if (m_pData != nullptr)
    {
        // Free the memory that was allocated dynamically.
        PAL_FREE(m_pData, m_pAllocator);
    }
}

} // Util
