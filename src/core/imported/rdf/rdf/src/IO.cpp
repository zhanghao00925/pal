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
#include "IO.h"

#include <zstd/zstd.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <algorithm>
// We use map so we don't have to provide a hash function for chunkId, which
// is a bit tricky with C++11 and old compilers
#include <map>
#include <vector>

#if RDF_PLATFORM_UNIX
    #include <sys/stat.h>
#endif

namespace rdf
{
namespace internal
{
    ///////////////////////////////////////////////////////////////////////////
    class RDF_EXPORT IStream
    {
    public:
        virtual ~IStream();

        std::size_t Read(const std::size_t count, void* buffer);
        std::size_t Write(const std::size_t count, const void* buffer);

        std::size_t Tell() const;
        void Seek(const std::size_t offset);
        std::size_t GetSize() const;

        bool CanWrite() const;
        bool CanRead() const;

    private:
        virtual std::size_t ReadImpl(const std::size_t count, void* buffer) = 0;
        virtual std::size_t WriteImpl(const std::size_t count, const void* buffer) = 0;
        virtual std::size_t TellImpl() const = 0;

        virtual void SeekImpl(const std::size_t offset) = 0;
        virtual std::size_t GetSizeImpl() const = 0;

        virtual bool CanWriteImpl() const = 0;
        virtual bool CanReadImpl() const = 0;
    };

    ///////////////////////////////////////////////////////////////////////////
    /**
	Helper class to make the handling of chunk IDs a bit easier
	*/
    class ChunkId final
    {
    public:
        using IdType = char[RDF_IDENTIFIER_SIZE];

        ChunkId()
        {
            ::memset(id_, 0, sizeof(IdType));
        }

        ChunkId(const char* id)
        {
            ::memset(id_, 0, sizeof(IdType));
            const auto size = ::strlen(id);
            assert(size <= sizeof(IdType));
            ::memcpy(id_, id, size);
        }

        ChunkId(const ChunkId& rhs)
        {
            ::memcpy(id_, rhs.id_, sizeof(IdType));
        }

        ChunkId& operator=(const ChunkId& rhs)
        {
            ::memcpy(id_, rhs.id_, sizeof(IdType));
            return *this;
        }

        bool operator<(const ChunkId& rhs) const
        {
            return ::memcmp(id_, rhs.id_, sizeof(IdType)) < 0;
        }

        bool operator==(const ChunkId& rhs) const
        {
            return ::memcmp(id_, rhs.id_, sizeof(IdType)) == 0;
        }

        bool operator!=(const ChunkId& rhs) const
        {
            return !(*this == rhs);
        }

    private:
        IdType id_;
    };

    static_assert(sizeof(ChunkId) == sizeof(char[RDF_IDENTIFIER_SIZE]),
                  "ChunkID size must match the chunk identifier size.");

    ///////////////////////////////////////////////////////////////////////////
    // Not part of C++11, so we need to do this manually
    template <typename T, typename... Args>
    std::unique_ptr<T> rdf_make_unique(Args&&... args)
    {
        return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
    }

    enum class AccessMode
    {
        Read,
        ReadWrite
    };

    std::unique_ptr<IStream> RDF_EXPORT OpenFile(const char* filename, AccessMode accessMode);
    std::unique_ptr<IStream> RDF_EXPORT CreateFile(const char* filename);

    std::unique_ptr<IStream> RDF_EXPORT CreateReadOnlyMemoryStream(const std::size_t bufferSize,
                                                                   const void* buffer);

    std::unique_ptr<IStream> RDF_EXPORT CreateMemoryStream();

    enum class Compression : std::uint8_t
    {
        None = 0,
        Zstd
    };

    ///////////////////////////////////////////////////////////////////////////
    class IChunkFileIterator
    {
    public:
        virtual ~IChunkFileIterator() = default;
        bool IsAtEnd() const;
        void Advance();
        void Get(char identifier[RDF_IDENTIFIER_SIZE], int* index) const;

    private:
        virtual bool IsAtEndImpl() const = 0;
        virtual void AdvanceImpl() = 0;
        virtual void GetImpl(char identifier[RDF_IDENTIFIER_SIZE], int* index) const = 0;
    };

    ///////////////////////////////////////////////////////////////////////////
    class ChunkFile final
    {
    public:
        struct IndexEntry final
        {
            char chunkIdentifier[RDF_IDENTIFIER_SIZE];
            // The following fields should add up to 4 bytes for alignment
            Compression compression;
            std::uint8_t reserved[3];

            // We'll assume one for all entries by default
            std::uint32_t version = 1;

            std::int64_t chunkHeaderOffset;
            std::int64_t chunkHeaderSize;
            std::int64_t chunkDataOffset;
            std::int64_t chunkDataSize;

            std::int64_t uncompressedChunkSize;
        };

        static_assert(sizeof(IndexEntry) == 64ULL, "Invalid index entry size.");

        static const char Identifier[];
        static const char LegacyIdentifier[];

        static constexpr int Version = 0x3;

        struct Header final
        {
            char identifier[8];  // "RTA_DATA" or "AMD_RDF "
            std::uint32_t version;
            std::uint32_t reserved;

            std::int64_t indexOffset;
            std::int64_t indexSize;
        };

        static_assert(sizeof(Header) == 32ULL, "Invalid header entry size.");

        ChunkFile(std::unique_ptr<IStream>&& stream)
            : streamPointer_(std::move(stream)), stream_(streamPointer_.get())
        {
            Construct();
        }

        ChunkFile(IStream* stream) : stream_(stream)
        {
            Construct();
        }

    private:
        void Construct()
        {
            if (stream_->Read(sizeof(header_), &header_) != sizeof(header_)) {
                throw std::runtime_error("Error while reading file -- could not read header");
            }

            if (::memcmp(header_.identifier, Identifier, 8) != 0
                && ::memcmp(header_.identifier, LegacyIdentifier, 8) != 0) {
                throw std::runtime_error("Invalid file header");
            }

            if (header_.version != Version) {
                throw std::runtime_error("Unsupported file version");
            }

            stream_->Seek(header_.indexOffset);
            index_.resize(header_.indexSize / sizeof(IndexEntry));
            stream_->Read(index_.size() * sizeof(IndexEntry), index_.data());

            BuildChunkIndex();
        }

    public:
        bool ContainsChunk(const char* chunkId, const int chunkIndex) const
        {
            assert(chunkId);
            assert(chunkIndex >= 0);

            ChunkId id(chunkId);

            auto it = chunkTypeRange_.find(id);
            if (it == chunkTypeRange_.end()) {
                throw std::runtime_error("Chunk not found");
            }

            if (chunkIndex >= (it->second.last - it->second.first)) {
                throw std::runtime_error("Chunk index out of range");
            }

            return true;
        }

        const IndexEntry& GetChunkInfo(const char* chunkId, const int chunkIndex) const
        {
            assert(chunkId);
            assert(chunkIndex >= 0);

            ChunkId id(chunkId);

            auto it = chunkTypeRange_.find(id);
            if (it == chunkTypeRange_.end()) {
                throw std::runtime_error("Chunk not found");
            }

            if (chunkIndex >= (it->second.last - it->second.first)) {
                throw std::runtime_error("Chunk index out of range");
            }

            return index_[it->second.first + chunkIndex];
        }

        const size_t GetChunkCount(const char* chunkId) const
        {
            assert(chunkId);

            ChunkId id(chunkId);

            auto it = chunkTypeRange_.find(id);
            if (it == chunkTypeRange_.end()) {
                return 0;
            }

            return it->second.last - it->second.first;
        }

        void ReadChunkHeader(const char* chunkId, const int chunkIndex, void* buffer)
        {
            const auto& entry = GetChunkInfo(chunkId, chunkIndex);

            assert(entry.chunkHeaderOffset >= 0);
            stream_->Seek(entry.chunkHeaderOffset);
            assert(entry.chunkHeaderSize >= 0);
            if (entry.chunkHeaderSize > 0) {
                // TODO Check error?
                stream_->Read(entry.chunkHeaderSize, buffer);
            }
        }

        void ReadChunkData(const char* chunkId, const int chunkIndex, void* buffer)
        {
            const auto& entry = GetChunkInfo(chunkId, chunkIndex);

            assert(entry.chunkDataOffset >= 0);
            stream_->Seek(entry.chunkDataOffset);

            assert(entry.chunkDataSize >= 0);
            if (entry.compression == Compression::Zstd) {
                std::vector<unsigned char> compressedData;
                compressedData.resize(entry.chunkDataSize);

                // TODO Check error?
                stream_->Read(entry.chunkDataSize, compressedData.data());
                assert(entry.uncompressedChunkSize >= 0);
                ZSTD_decompress(buffer,
                                entry.uncompressedChunkSize,
                                compressedData.data(),
                                compressedData.size());
            } else {
                // TODO Check error?
                stream_->Read(entry.chunkDataSize, buffer);
            }
        }

        std::uint32_t GetChunkVersion(const char* chunkId, const int index) const
        {
            return GetChunkInfo(chunkId, index).version;
        }

        std::size_t GetChunkDataSize(const char* chunkId, const int index) const
        {
            const auto& info = GetChunkInfo(chunkId, index);
            if (info.compression != Compression::None) {
                return info.uncompressedChunkSize;
            } else {
                return info.chunkDataSize;
            }
        }

        std::size_t GetChunkHeaderSize(const char* chunkId, const int index) const
        {
            return GetChunkInfo(chunkId, index).chunkHeaderSize;
        }

    private:
        void BuildChunkIndex()
        {
            // We stable-sort this by index name. This allows us to index
            // consecutive entries with the same name quickly
            std::stable_sort(index_.begin(),
                             index_.end(),
                             [](const ChunkFile::IndexEntry& first,
                                const ChunkFile::IndexEntry& second) -> bool {
                                 return ::memcmp(first.chunkIdentifier,
                                                 second.chunkIdentifier,
                                                 sizeof(first.chunkIdentifier)) < 0;
                             });

            ChunkId currentChunkId;
            std::size_t start = 0;
            for (std::size_t current = 0; current < index_.size(); ++current) {
                const ChunkId id(index_[current].chunkIdentifier);

                // New chunks are starting from here on out
                if (id != currentChunkId) {
                    if (current != start) {
                        chunkTypeRange_[currentChunkId] = Range(start, current);
                    }

                    currentChunkId = id;
                    start = current;
                }
            }

            if (start < index_.size()) {
                chunkTypeRange_[currentChunkId] = Range(start, index_.size());
            }
        }

        Header header_;
        std::vector<IndexEntry> index_;

        struct Range
        {
            Range() = default;
            Range(const std::size_t first, const std::size_t last) : first(first), last(last) {}

            std::size_t first = 0;
            std::size_t last = 0;
        };

        // For each chunk type, store the range of entries inside index_
        // The tuple contains a range [first, last)
        std::map<ChunkId, Range> chunkTypeRange_;

        // If we own the stream, this will be non-null
        std::unique_ptr<IStream> streamPointer_;
        IStream* stream_ = nullptr;

        class ChunkFileIterator final : public IChunkFileIterator
        {
        public:
            ChunkFileIterator(const std::map<ChunkId, Range>* range) : chunkTypeRange_(range)
            {
                it_ = chunkTypeRange_->begin();
                currentEntry_ = 0;
            }

        private:
            void AdvanceImpl()
            {
                if (IsAtEnd()) {
                    return;
                }

                ++currentEntry_;
                if (currentEntry_ >= (it_->second.last - it_->second.first)) {
                    ++it_;
                    currentEntry_ = 0;
                }
            }

            void GetImpl(char* name, int* index) const
            {
                if (name) {
                    ::memcpy(name, &it_->first, sizeof(it_->first));
                }

                if (index) {
                    *index = currentEntry_;
                }
            }

            bool IsAtEndImpl() const
            {
                return it_ == chunkTypeRange_->end();
            }

            const std::map<ChunkId, Range>* chunkTypeRange_;
            std::map<ChunkId, Range>::const_iterator it_;
            int currentEntry_ = 0;
        };

    public:
        std::unique_ptr<ChunkFileIterator> GetIterator() const
        {
            return rdf_make_unique<ChunkFileIterator>(&chunkTypeRange_);
        }
    };

    const char ChunkFile::LegacyIdentifier[] = {'R', 'T', 'A', '_', 'D', 'A', 'T', 'A'};
    const char ChunkFile::Identifier[] = {'A', 'M', 'D', '_', 'R', 'D', 'F', ' '};

    ///////////////////////////////////////////////////////////////////////////
    class ChunkFileWriter final
    {
    public:
        ChunkFileWriter(std::unique_ptr<IStream>&& stream)
            : streamPointer_(std::move(stream)), stream_(streamPointer_.get())
        {
            Construct();
        }

        ChunkFileWriter(IStream* stream) : stream_(stream)
        {
            Construct();
        }

        void BeginChunk(const char* chunkIdentifier,
                        const size_t chunkHeaderSize,
                        const void* chunkHeader,
                        const Compression compression,
                        const std::uint32_t version)
        {
            assert(currentChunk_ == nullptr);

            ChunkFile::IndexEntry entry;
            ::memset(&entry, 0, sizeof(entry));

            if (::strlen(chunkIdentifier) > sizeof(entry.chunkIdentifier)) {
                throw std::runtime_error("Chunk identifier must be <= 16 characters in length.");
            }
            // Without trailing \0!
            ::memcpy(entry.chunkIdentifier, chunkIdentifier, ::strlen(chunkIdentifier));

            entry.compression = compression;
            entry.version = version;

            chunks_.push_back(entry);
            currentChunk_ = &chunks_.back();

            currentChunk_->chunkHeaderOffset = dataWriteOffset_;
            assert(currentChunk_->chunkHeaderOffset >= 0);
            if (chunkHeaderSize > 0) {
                // TODO Check error?
                stream_->Write(chunkHeaderSize, chunkHeader);
                currentChunk_->chunkHeaderSize = chunkHeaderSize;

                // If this fails, we had an overflow in the chunk header size
                assert(currentChunk_->chunkHeaderSize >= 0);
            }
            dataWriteOffset_ += chunkHeaderSize;

            currentChunk_->chunkDataOffset = dataWriteOffset_;
            assert(currentChunk_->chunkDataOffset >= 0);
        }

        void AppendToChunk(const size_t chunkDataSize, const void* chunkData)
        {
            assert(currentChunk_);

            if (currentChunk_->compression != Compression::None) {
                chunkDataBuffer_.insert(
                    chunkDataBuffer_.end(),
                    static_cast<const unsigned char*>(chunkData),
                    static_cast<const unsigned char*>(chunkData) + chunkDataSize);
            } else {
                if (stream_->Write(chunkDataSize, chunkData) != chunkDataSize) {
                    throw std::runtime_error("Error while writing to file.");
                }

                dataWriteOffset_ += chunkDataSize;
            }
        }

        int EndChunk()
        {
            if (currentChunk_->compression != Compression::None) {
                std::vector<unsigned char> buffer;
                buffer.resize(ZSTD_compressBound(chunkDataBuffer_.size()));

                const auto compressedSize = ZSTD_compress(buffer.data(),
                                                          buffer.size(),
                                                          chunkDataBuffer_.data(),
                                                          chunkDataBuffer_.size(),
                                                          ZSTD_CLEVEL_DEFAULT);

                currentChunk_->chunkDataSize = compressedSize;
                assert(currentChunk_->chunkDataSize >= 0);
                currentChunk_->uncompressedChunkSize = chunkDataBuffer_.size();
                assert(currentChunk_->uncompressedChunkSize >= 0);

                stream_->Write(compressedSize, buffer.data());
                dataWriteOffset_ += compressedSize;
            } else {
                assert(currentChunk_->chunkDataOffset >= 0);
                currentChunk_->chunkDataSize = dataWriteOffset_ - currentChunk_->chunkDataOffset;
                assert(currentChunk_->chunkDataSize >= 0);
            }

            ChunkId id(currentChunk_->chunkIdentifier);

            int index = 0;
            if (chunkCountPerType_.find(id) != chunkCountPerType_.end()) {
                auto entry = chunkCountPerType_.find(id);
                index = entry->second;
                ++entry->second;
            } else {
                chunkCountPerType_[id] = 1;
            }

            currentChunk_ = nullptr;
            chunkDataBuffer_.clear();

            return index;
        }

        int WriteChunk(const char* chunkIdentifier,
                       const size_t chunkHeaderSize,
                       const void* chunkHeader,
                       const size_t chunkDataSize,
                       const void* chunkData,
                       const Compression compression,
                       const std::uint32_t version)
        {
            BeginChunk(chunkIdentifier, chunkHeaderSize, chunkHeader, compression, version);
            AppendToChunk(chunkDataSize, chunkData);
            return EndChunk();
        }

        /**
        Flush all pending data and finalize the file.

        This function must be called exactly once per instantation. It's not
        automatically called in the destructor as it could throw.
        */
        void Finalize()
        {
            assert(stream_);
            stream_->Write(chunks_.size() * sizeof(ChunkFile::IndexEntry), chunks_.data());
            stream_->Seek(0);
            header_.indexOffset = dataWriteOffset_;
            header_.indexSize = chunks_.size() * sizeof(ChunkFile::IndexEntry);

            // TODO Check error?
            stream_->Write(sizeof(header_), &header_);

            stream_ = nullptr;
        }

    private:
        void Construct()
        {
            header_.version = ChunkFile::Version;
            ::memcpy(header_.identifier, ChunkFile::Identifier, sizeof(ChunkFile::Identifier));
            header_.reserved = 0;

            stream_->Write(sizeof(header_), &header_);
            dataWriteOffset_ = stream_->Tell();
        }

        std::vector<ChunkFile::IndexEntry> chunks_;
        std::vector<unsigned char> chunkDataBuffer_;

        std::map<ChunkId, int> chunkCountPerType_;

        ChunkFile::IndexEntry* currentChunk_ = nullptr;
        ChunkFile::Header header_;
        std::unique_ptr<IStream> streamPointer_;
        IStream* stream_ = nullptr;

        size_t dataWriteOffset_ = 0;
    };

    //////////////////////////////////////////////////////////////////////
    IStream::~IStream() {}

    //////////////////////////////////////////////////////////////////////
    std::size_t IStream::Read(const std::size_t size, void* buffer)
    {
        if (size > 0 && buffer == nullptr) {
            throw std::runtime_error("Buffer cannot be null");
        }

        return ReadImpl(size, buffer);
    }

    //////////////////////////////////////////////////////////////////////
    std::size_t IStream::Write(const std::size_t size, const void* buffer)
    {
        if (size > 0 && buffer == nullptr) {
            throw std::runtime_error("Buffer cannot be null");
        }

        return WriteImpl(size, buffer);
    }

    //////////////////////////////////////////////////////////////////////
    bool IStream::CanRead() const
    {
        return CanReadImpl();
    }

    //////////////////////////////////////////////////////////////////////
    bool IStream::CanWrite() const
    {
        return CanWriteImpl();
    }

    //////////////////////////////////////////////////////////////////////
    std::size_t IStream::Tell() const
    {
        return TellImpl();
    }

    //////////////////////////////////////////////////////////////////////
    std::size_t IStream::GetSize() const
    {
        return GetSizeImpl();
    }

    //////////////////////////////////////////////////////////////////////
    void IStream::Seek(const std::size_t offset)
    {
        SeekImpl(offset);
    }

    //////////////////////////////////////////////////////////////////////
    class Filestream final : public IStream
    {
    public:
        Filestream(std::FILE* fd, AccessMode accessMode) : fd_(fd), accessMode_(accessMode) {}

        ~Filestream()
        {
            std::fclose(fd_);
        }

    private:
        std::size_t ReadImpl(const std::size_t count, void* buffer) override
        {
            return std::fread(buffer, 1, count, fd_);
        }

        std::size_t WriteImpl(const std::size_t count, const void* buffer) override
        {
            return std::fwrite(buffer, 1, count, fd_);
        }

        std::size_t TellImpl() const override
        {
#if RDF_PLATFORM_WINDOWS
            return _ftelli64(fd_);
#elif RDF_PLATFORM_UNIX
            // Linux ftell returns a long, which is 64-bit
            return ftell(fd_);
#else
#error "Unsupported platform"
#endif
        }

        void SeekImpl(const std::size_t offset) override
        {
#if RDF_PLATFORM_WINDOWS
            _fseeki64(fd_, offset, SEEK_SET);
#elif RDF_PLATFORM_UNIX
            // Linux fseek is a long, so 64-bit
            fseek(fd_, offset, SEEK_SET);
#else
    #error "Unsupported platform"
#endif
        }

        bool CanWriteImpl() const override
        {
            return accessMode_ == AccessMode::ReadWrite;
        }

        bool CanReadImpl() const override
        {
            return true;
        }

        std::size_t GetSizeImpl() const override
        {
#if RDF_PLATFORM_WINDOWS
            struct _stat64 statBuffer;
            _fstat64(_fileno(fd_), &statBuffer);
            return statBuffer.st_size;
#elif RDF_PLATFORM_UNIX
            struct stat statBuffer;
            fstat(fileno(fd_), &statBuffer);
            return statBuffer.st_size;
#else
    #error "Unsupported platform"
#endif
        }

        std::FILE* fd_;
        AccessMode accessMode_;
    };

    //////////////////////////////////////////////////////////////////////
    class ReadOnlyMemoryStream final : public IStream
    {
    public:
        ReadOnlyMemoryStream(const std::size_t size, const void* buffer)
            : size_(size), buffer_(buffer)
        {
        }

    private:
        std::size_t ReadImpl(const std::size_t count, void* buffer) override
        {
            const auto startPointer = readPointer_;
            auto endPointer = readPointer_ + count;
            if (endPointer > size_) {
                endPointer = size_;
            }

            if (endPointer - startPointer) {
                ::memcpy(buffer,
                         static_cast<const unsigned char*>(buffer_) + startPointer,
                         endPointer - startPointer);
            }

            readPointer_ = endPointer;
            return endPointer - startPointer;
        }

        std::size_t WriteImpl(const std::size_t count, const void* buffer) override
        {
            assert(false);
            return 0;
        }

        std::size_t TellImpl() const override
        {
            return readPointer_;
        }

        std::size_t GetSizeImpl() const override
        {
            return size_;
        }

        void SeekImpl(const std::size_t offset) override
        {
            if (offset >= size_) {
                throw std::runtime_error("Seek out-of-bounds");
            }

            readPointer_ = offset;
        }

        bool CanWriteImpl() const override
        {
            return false;
        }

        bool CanReadImpl() const override
        {
            return true;
        }

        std::size_t size_;
        std::size_t readPointer_ = 0;
        const void* buffer_;
    };

    //////////////////////////////////////////////////////////////////////
    class MemoryStream final : public IStream
    {
    private:
        std::size_t ReadImpl(const std::size_t count, void* buffer) override
        {
            // This is all prone to overflow, but we try to apply mostly "safe"
            // math operations here
            auto end = offset_ + count;

            if (end > data_.size()) {
                end = data_.size();
            }

            auto bytesToRead = end - offset_;
            ::memcpy(buffer, data_.data() + offset_, bytesToRead);
            offset_ += bytesToRead;
            return bytesToRead;
        }

        std::size_t WriteImpl(const std::size_t count, const void* buffer) override
        {
            const auto end = offset_ + count;
            if (end > data_.size()) {
                data_.resize(end);
            }

            ::memcpy(data_.data() + offset_, buffer, count);
            offset_ += count;
            return count;
        }

        std::size_t TellImpl() const override
        {
            return offset_;
        }

        std::size_t GetSizeImpl() const override
        {
            return data_.size();
        }

        void SeekImpl(const std::size_t offset) override
        {
            offset_ = offset;

            // Resize the file if we seek beyond the end
            if (offset_ > data_.size()) {
                data_.resize(offset_);
            }
        }

        bool CanWriteImpl() const override
        {
            return true;
        }

        bool CanReadImpl() const override
        {
            return true;
        }

        std::vector<unsigned char> data_;
        std::size_t offset_ = 0;
    };

    //////////////////////////////////////////////////////////////////////
    std::unique_ptr<IStream> CreateReadOnlyMemoryStream(const std::size_t size, const void* buffer)
    {
        return rdf_make_unique<ReadOnlyMemoryStream>(size, buffer);
    }

    //////////////////////////////////////////////////////////////////////
    std::unique_ptr<IStream> CreateMemoryStream()
    {
        return rdf_make_unique<MemoryStream>();
    }

    //////////////////////////////////////////////////////////////////////
    std::unique_ptr<IStream> OpenFile(const char* filename, AccessMode accessMode)
    {
        auto fd = std::fopen(filename, accessMode == AccessMode::Read ? "rb" : "wb");
        if (fd == nullptr) {
            throw std::runtime_error("Could not open file");
        }

        return rdf_make_unique<Filestream>(fd, accessMode);
    }

    //////////////////////////////////////////////////////////////////////
    std::unique_ptr<IStream> CreateFile(const char* filename)
    {
        auto fd = std::fopen(filename, "wb");
        if (fd == nullptr) {
            throw std::runtime_error("Could not create file");
        }

        return rdf_make_unique<Filestream>(fd, AccessMode::ReadWrite);
    }

    ///////////////////////////////////////////////////////////////////////////
    bool IChunkFileIterator::IsAtEnd() const
    {
        return IsAtEndImpl();
    }

    ///////////////////////////////////////////////////////////////////////////
    void IChunkFileIterator::Advance()
    {
        return AdvanceImpl();
    }

    ///////////////////////////////////////////////////////////////////////////
    void IChunkFileIterator::Get(char identifier[RDF_IDENTIFIER_SIZE], int* index) const
    {
        GetImpl(identifier, index);
    }
}  // namespace internal
}  // namespace rdf

#define RDF_C_API_BEGIN \
    try {
#define RDF_C_API_END                     \
    }                                     \
    catch (...) {                         \
        return rdfResult::rdfResultError; \
    }

struct rdfChunkFile
{
    std::unique_ptr<rdf::internal::ChunkFile> chunkFile;
};

struct rdfStream
{
    std::unique_ptr<rdf::internal::IStream> stream;
};

struct rdfChunkFileIterator
{
    std::unique_ptr<rdf::internal::IChunkFileIterator> iterator;
};

struct rdfChunkFileWriter
{
    std::unique_ptr<rdf::internal::ChunkFileWriter> writer;
};

//////////////////////////////////////////////////////////////////////////////
/**
Open a file for reading.

This creates a new stream from the given file path.
*/
int RDF_EXPORT rdfStreamOpenFile(const char* filename, rdfStream** handle)
{
    RDF_C_API_BEGIN

    *handle = new rdfStream;
    try {
        (*handle)->stream = rdf::internal::OpenFile(filename, rdf::internal::AccessMode::Read);
    } catch (...) {
        delete *handle;
        throw;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Create a new file and open it for read/write access.

This creates a new stream which points to a newly created file.
*/
int RDF_EXPORT rdfStreamCreateFile(const char* filename, rdfStream** handle)
{
    RDF_C_API_BEGIN

    *handle = new rdfStream;
    try {
        (*handle)->stream = rdf::internal::CreateFile(filename);
    } catch (...) {
        delete *handle;
        throw;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Create a stream from read-only memory.

This creates a new stream which is backed by the provided memory. This is
useful for testing, as you can load data directly from memory.
*/
int RDF_EXPORT rdfStreamFromReadOnlyMemory(const std::size_t size,
                                           const void* data,
                                           rdfStream** handle)
{
    RDF_C_API_BEGIN

    *handle = new rdfStream;
    try {
        (*handle)->stream = rdf::internal::CreateReadOnlyMemoryStream(size, data);
    } catch (...) {
        delete *handle;
        throw;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Create a read/write in-memory stream.

This is useful to stage data in memory -- for example for tests, or to
prepare data before writing to disk.
*/
int RDF_EXPORT rdfStreamCreateMemoryStream(rdfStream** handle)
{
    RDF_C_API_BEGIN

    *handle = new rdfStream;
    try {
        (*handle)->stream = rdf::internal::CreateMemoryStream();
    } catch (...) {
        delete *handle;
        throw;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Close and destroy a stream.

The handle will be reset.
*/
int RDF_EXPORT rdfStreamClose(rdfStream** handle)
{
    RDF_C_API_BEGIN

    if (handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (*handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    delete *handle;
    *handle = nullptr;

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Read a number of bytes from the stream.

If bytesRead is non-null, the number of bytes read will be stored there.
*/
int RDF_EXPORT rdfStreamRead(rdfStream* stream,
                             const std::size_t count,
                             void* buffer,
                             std::size_t* bytesRead)
{
    RDF_C_API_BEGIN

    if (stream == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    auto read = stream->stream->Read(count, buffer);
    if (bytesRead) {
        *bytesRead = read;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Write a number of bytes to the stream.

If bytesWritten is non-null, the number of bytes written will be stored there.
*/
int RDF_EXPORT rdfStreamWrite(rdfStream* stream,
                              const std::size_t count,
                              const void* buffer,
                              std::size_t* bytesWritten)
{
    RDF_C_API_BEGIN

    if (stream == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    auto written = stream->stream->Write(count, buffer);
    if (bytesWritten) {
        *bytesWritten = written;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

int RDF_EXPORT rdfStreamTell(rdfStream* stream, std::size_t* position)
/**
Get the current stream position.
*/
{
    RDF_C_API_BEGIN

    if (stream == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (position == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    *position = stream->stream->Tell();

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Set the current stream position.
*/
int RDF_EXPORT rdfStreamSeek(rdfStream* stream, const std::size_t offset)
{
    RDF_C_API_BEGIN

    if (stream == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    stream->stream->Seek(offset);

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Get the size of the stream.
*/
int RDF_EXPORT rdfStreamGetSize(rdfStream* stream, std::size_t* size)
{
    RDF_C_API_BEGIN

    if (stream == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (size == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    *size = stream->stream->GetSize();

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Create a read-only chunk file from an existing file.
*/
int RDF_EXPORT rdfChunkFileOpenFile(const char* filename, rdfChunkFile** handle)
{
    RDF_C_API_BEGIN

    *handle = new rdfChunkFile;
    try {
        (*handle)->chunkFile.reset(new rdf::internal::ChunkFile(
            rdf::internal::OpenFile(filename, rdf::internal::AccessMode::Read)));
    } catch (...) {
        delete *handle;
        throw;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Create a read-only chunk file from an existing stream.
*/
int RDF_EXPORT rdfChunkFileOpenStream(rdfStream* stream, rdfChunkFile** handle)
{
    RDF_C_API_BEGIN

    *handle = new rdfChunkFile;
    try {
        (*handle)->chunkFile.reset(new rdf::internal::ChunkFile(stream->stream.get()));
    } catch (...) {
        delete *handle;
        throw;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Close a chunk file.

This resets the handle as well.
*/
int RDF_EXPORT rdfChunkFileClose(rdfChunkFile** handle)
{
    RDF_C_API_BEGIN

    if (handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (*handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    delete *handle;
    *handle = nullptr;

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Get the version field of a chunk stored in the chunk file.
*/
int RDF_EXPORT rdfChunkFileGetChunkVersion(rdfChunkFile* handle,
                                           const char* chunkId,
                                           const int chunkIndex,
                                           std::uint32_t* chunkVersion)
{
    RDF_C_API_BEGIN

    if (handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (chunkVersion == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    *chunkVersion = handle->chunkFile->GetChunkVersion(chunkId, chunkIndex);

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Read the data stored in a chunk into the provided buffer.

The correct buffer size can be obtained using rdfChunkFileGetChunkDataSize. The
user must allocate at least that many bytes.
*/
int RDF_EXPORT rdfChunkFileReadChunkData(rdfChunkFile* handle,
                                         const char* chunkId,
                                         const int chunkIndex,
                                         void* buffer)
{
    RDF_C_API_BEGIN

    if (handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (buffer == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    handle->chunkFile->ReadChunkData(chunkId, chunkIndex, buffer);

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Read the chunk header into the provided buffer.

The correct buffer size can be obtained using rdfChunkFileGetChunkHeaderSize. The
user must allocate at least that many bytes.
*/
int RDF_EXPORT rdfChunkFileReadChunkHeader(rdfChunkFile* handle,
                                           const char* chunkId,
                                           const int chunkIndex,
                                           void* buffer)
{
    RDF_C_API_BEGIN

    if (handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (buffer == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    handle->chunkFile->ReadChunkHeader(chunkId, chunkIndex, buffer);

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Get the size of the chunk header.
*/
int RDF_EXPORT rdfChunkFileGetChunkHeaderSize(rdfChunkFile* handle,
                                              const char* chunkId,
                                              const int chunkIndex,
                                              std::size_t* size)
{
    RDF_C_API_BEGIN

    if (handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (size == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    *size = handle->chunkFile->GetChunkHeaderSize(chunkId, chunkIndex);

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Get the size of the chunk data.
*/
int RDF_EXPORT rdfChunkFileGetChunkDataSize(rdfChunkFile* handle,
                                            const char* chunkId,
                                            const int chunkIndex,
                                            std::size_t* size)
{
    RDF_C_API_BEGIN

    if (handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (size == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    *size = handle->chunkFile->GetChunkDataSize(chunkId, chunkIndex);

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Get the number of chunks using the provided chunk id.
*/
int RDF_EXPORT rdfChunkFileGetChunkCount(rdfChunkFile* handle,
                                         const char* chunkId,
                                         std::size_t* size)
{
    RDF_C_API_BEGIN

    if (handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (size == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    *size = handle->chunkFile->GetChunkCount(chunkId);

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Check if the file contains a specified chunk id.
*/
int RDF_EXPORT rdfChunkFileContainsChunk(rdfChunkFile* handle,
                                         const char* chunkId,
                                         const int chunkIndex,
                                         int* result)
{
    RDF_C_API_BEGIN

    if (handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (result == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    *result = handle->chunkFile->ContainsChunk(chunkId, chunkIndex) ? 1 : 0;

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Create a chunk file iterator.

The chunk file iterator will iterate over all chunks in a file. The order of
chunk identifiers is undefined, but within a chunk identifier, it will iterate
all elements before moving on to the next chunk.

For instance, in a file with two chunks with the idenfier ID1, and one chunk
with the identifier ID2, the iterator can start with either ID1 or ID2. It will
however always enumerate the two ID1 chunks in-order (i.e. ID1, 0 followed by
ID1, 1.)
*/
int RDF_EXPORT rdfChunkFileCreateChunkIterator(rdfChunkFile* handle,
                                               rdfChunkFileIterator** iterator)
{
    RDF_C_API_BEGIN

    if (handle == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (iterator == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    *iterator = new rdfChunkFileIterator;
    try {
        (*iterator)->iterator = handle->chunkFile->GetIterator();
    } catch (...) {
        delete *iterator;
        throw;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Destroy a chunk iterator.

This also resets the handle.
*/
int RDF_EXPORT rdfChunkFileDestroyChunkIterator(rdfChunkFileIterator** iterator)
{
    RDF_C_API_BEGIN

    if (iterator == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (*iterator == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    delete *iterator;
    *iterator = nullptr;

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Advance a chunk file iterator by one.

Iterating on an iterator which is at the end already is a no-op.
*/
int RDF_EXPORT rdfChunkFileIteratorAdvance(rdfChunkFileIterator* iterator)
{
    RDF_C_API_BEGIN

    if (iterator == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    iterator->iterator->Advance();

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Check if a chunk file iterator has reached the end.
*/
int RDF_EXPORT rdfChunkFileIteratorIsAtEnd(rdfChunkFileIterator* iterator, int* atEnd)
{
    RDF_C_API_BEGIN

    if (iterator == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (atEnd == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    *atEnd = iterator->iterator->IsAtEnd() ? 1 : 0;

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Get the identifier of the current chunk.

Note that the identifier alone is not sufficient to uniquely identify a chunk.
Use rdfChunkFileIteratorGetChunkIndex to get the index as well which allows to
uniquely identify a chunk.
*/
int RDF_EXPORT rdfChunkFileIteratorGetChunkIdentifier(rdfChunkFileIterator* iterator,
                                                      char identifier[RDF_IDENTIFIER_SIZE])
{
    RDF_C_API_BEGIN

    if (iterator == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (identifier == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    iterator->iterator->Get(identifier, nullptr);

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Get the index of the current chunk.

Note that the index alone is not sufficient to uniquely identify a chunk.
Use rdfChunkFileIteratorGetChunkIdentifier to get the identifier as well which
allows to uniquely identify a chunk.
*/
int RDF_EXPORT rdfChunkFileIteratorGetChunkIndex(rdfChunkFileIterator* iterator, int* index)
{
    RDF_C_API_BEGIN

    if (iterator == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (index == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    iterator->iterator->Get(nullptr, index);

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Create a new chunk file writer.

The stream must allow both reads and writes.
*/
int RDF_EXPORT rdfChunkFileWriterCreate(rdfStream* stream, rdfChunkFileWriter** writer)
{
    RDF_C_API_BEGIN

    if (stream == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (writer == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    *writer = new rdfChunkFileWriter;
    try {
        (*writer)->writer.reset(new rdf::internal::ChunkFileWriter(stream->stream.get()));
    } catch (...) {
        delete *writer;
        throw;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Destroy a chunk file writer.

This flushes all pending data and destroys the writer.

This function also resets the handle. If the writing fails, the handle will be
destroyed but an error code will be returned.
*/
int RDF_EXPORT rdfChunkFileWriterDestroy(rdfChunkFileWriter** writer)
{
    RDF_C_API_BEGIN

    if (writer == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (*writer == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    rdfResult result = rdfResult::rdfResultOk;

    try {
        // We finalize in a separate step just in case it throws an exception
        (*writer)->writer->Finalize();
    } catch (...) {
        // Writing failed. We still need to clean up, but we'll return an
        // error.
        result = rdfResult::rdfResultError;
    }

    delete *writer;
    *writer = nullptr;

    return result;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Begin writing of a new chunk.

A version == 0 will be bumped to 1 automatically, which is the default version.
This makes it easier to use a default-initalized create info.

After you begin a chunk, use rdfChunkFileWriterAppendToChunk to append data to
it.
*/
int RDF_EXPORT rdfChunkFileWriterBeginChunk(rdfChunkFileWriter* writer,
                                            const rdfChunkCreateInfo* info)
{
    RDF_C_API_BEGIN

    if (writer == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (info == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    writer->writer->BeginChunk(info->identifier,
                               info->headerSize,
                               info->pHeader,
                               static_cast<rdf::internal::Compression>(info->compression),
                               info->version == 0 ? 1 : info->version);

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Append data to the current chunk.

To begin a chunk, use rdfChunkFileWriterBeginChunk. Once done, call
rdfChunkFileWriterEndChunk which finalizes the current chunk.
*/
int RDF_EXPORT rdfChunkFileWriterAppendToChunk(rdfChunkFileWriter* writer,
                                               const std::size_t size,
                                               const void* data)
{
    RDF_C_API_BEGIN

    if (writer == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    writer->writer->AppendToChunk(size, data);

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Finish writing a chunk.

If index is non-null, the chunk index is stored there. Creating a chunk with
the same name will result in consecutive indices being assigned.
*/
int RDF_EXPORT rdfChunkFileWriterEndChunk(rdfChunkFileWriter* writer, int* index)
{
    RDF_C_API_BEGIN

    if (writer == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    const int chunkIndex = writer->writer->EndChunk();
    if (index) {
        *index = chunkIndex;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Convenience function to write a chunk in a single call.

This is equivalent to Begin/Append/End, but does it all at the same time.
See rdfChunkFileWriterBeginChunk, rdfChunkFileWriterAppendToChunk and
rdfChunkFileWriterEndChunk.
*/
int RDF_EXPORT rdfChunkFileWriterWriteChunk(rdfChunkFileWriter* writer,
                                            const rdfChunkCreateInfo* info,
                                            const std::size_t size,
                                            const void* data,
                                            int* index)
{
    RDF_C_API_BEGIN

    if (writer == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    if (info == nullptr) {
        return rdfResult::rdfResultInvalidArgument;
    }

    // We call the C++ version here, as we don't want to swallow intermediate
    // exceptions
    const auto chunkIndex =
        writer->writer->WriteChunk(info->identifier,
                                   info->headerSize,
                                   info->pHeader,
                                   size,
                                   data,
                                   static_cast<rdf::internal::Compression>(info->compression),
                                   info->version == 0 ? 1 : info->version);

    if (index) {
        *index = chunkIndex;
    }

    return rdfResult::rdfResultOk;

    RDF_C_API_END
}

//////////////////////////////////////////////////////////////////////////////
/**
Convert a rdfResult to a human-readable string.
*/
int RDF_EXPORT rdfResultToString(rdfResult result, const char** output)
{
    switch (result) {
    case rdfResultOk:
        *output = "RDF: No error";
        break;
    case rdfResultError:
        *output = "RDF: Error";
        break;
    case rdfResultInvalidArgument:
        *output = "RDF: Invalid argument";
        break;

    default:
        return rdfResultError;
    }

    return rdfResult::rdfResultOk;
}
