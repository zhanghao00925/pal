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
#include <cli11/CLI11.hpp>
#include <json/json.hpp>

#include "IO.h"

namespace
{
std::vector<std::byte> ConvertArrayToBytes(const nlohmann::json& object)
{
    if (! object.is_array()) {
        throw std::runtime_error("Object must be an array.");
    }

    std::vector<std::byte> result;
    result.reserve(object.size());

    for (const auto& element : object) {
        std::byte b;
        if (element.is_number_unsigned()) {
            const auto v = element.get<std::uint32_t>();

            if (v > 255) {
                throw std::runtime_error("Byte array values must be within 0..255");
            }

            b = static_cast<std::byte>(v);
        } else if (element.is_number_integer()) {
            const auto v = element.get<std::int32_t>();

            if (v < 0 || v > 255) {
                throw std::runtime_error("Byte array values must be within 0..255");
            }

            b = static_cast<std::byte>(v);
        } else {
            throw std::runtime_error("Byte array must consist of integer numbers only.");
        }

        result.push_back(b);
    }

    return result;
}

int CreateChunkFile(const std::string& input, const std::string& output,
    const bool verbose)
{
    auto inputStream = rdf::Stream::OpenFile(input.c_str());
    std::vector<char> buffer(inputStream.GetSize());
    inputStream.Read(buffer.size(), buffer.data());

    auto outputStream = rdf::Stream::CreateFile(output.c_str());
    rdf::ChunkFileWriter writer(outputStream);

    nlohmann::json config = nlohmann::json::parse(buffer.begin(), buffer.end());

    for (const auto& chunk : config["chunks"]) {
        const auto id = chunk["id"].get<std::string>();
        int version = 1;
        if (chunk.contains("version")) {
            version = chunk["version"].get<int>();
        }

        rdfCompression compression = rdfCompressionNone;
        if (chunk.contains("compression")) {
            const auto c = chunk["compression"].get<std::string>();
            if (c == "zstd" || c == "zstandard") {
                compression = rdfCompressionZstd;
            }
        }

        if (chunk.contains("header")) {
            const auto header = ConvertArrayToBytes(chunk["header"]);

            writer.BeginChunk(id.c_str(), header.size(), header.data(),
                compression, version);

            if (verbose) {
                std::cout << "Begin chunk: '" << id << "' with header, compression: " << compression
                          << ", version: " << version << "\n";
            }
        } else {
            writer.BeginChunk(id.c_str(), 0, nullptr, compression, version);

            if (verbose) {
                std::cout << "Begin chunk: '" << id
                          << "' without header, compression: " << compression
                          << ", version: " << version << "\n";
            }
        }

        if (chunk.contains("data")) {
            const auto data = ConvertArrayToBytes(chunk["data"]);

            writer.AppendToChunk(data.size(), data.data());

            if (verbose) {
                std::cout << "Writing data"
                          << "\n";
            }
        }

        writer.EndChunk();

        if (verbose) {
            std::cout << "End chunk: '" << id << "'"
                      << "\n";
        }
    }

    writer.Close();

    return 0;
}
}  // namespace

int main(int argc, char* argv[])
{
    CLI::App app{"RDFG 1.0"};

    std::string input, output;
    bool verbose = false;

    auto createChunkFile =
        app.add_subcommand("create", "Create a chunk file from the provided input");
    createChunkFile->add_option("input", input)->required();
    createChunkFile->add_option("output", output)->required();
    createChunkFile->add_flag("-v,--verbose", verbose);

    CLI11_PARSE(app, argc, argv);

    try {
        if (*createChunkFile) {
            return CreateChunkFile(input, output, verbose);
        }
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
    }

    return 0;
}