// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "inputs_filling.hpp"

#include <format_reader_ptr.h>

#include <algorithm>
#include <memory>
#include <samples/slog.hpp>
#include <string>
#include <utility>
#include <vector>

#include "shared_blob_allocator.h"

using namespace InferenceEngine;

#ifdef USE_OPENCV
static const std::vector<std::string> supported_image_extensions =
    {"bmp", "dib", "jpeg", "jpg", "jpe", "jp2", "png", "pbm", "pgm", "ppm", "sr", "ras", "tiff", "tif"};
#else
static const std::vector<std::string> supported_image_extensions = {"bmp"};
#endif
static const std::vector<std::string> supported_binary_extensions = {"bin"};

std::vector<std::string> filterFilesByExtensions(const std::vector<std::string>& filePaths,
                                                 const std::vector<std::string>& extensions) {
    std::vector<std::string> filtered;
    auto getExtension = [](const std::string& name) {
        auto extensionPosition = name.rfind('.', name.size());
        return extensionPosition == std::string::npos ? "" : name.substr(extensionPosition + 1, name.size() - 1);
    };
    for (auto& filePath : filePaths) {
        auto extension = getExtension(filePath);
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
        if (std::find(extensions.begin(), extensions.end(), extension) != extensions.end()) {
            filtered.push_back(filePath);
        }
    }
    return filtered;
}

template <typename T>
using uniformDistribution = typename std::conditional<
    std::is_floating_point<T>::value,
    std::uniform_real_distribution<T>,
    typename std::conditional<std::is_integral<T>::value, std::uniform_int_distribution<T>, void>::type>::type;

template <typename T>
InferenceEngine::Blob::Ptr createBlobFromImage(const std::vector<std::string>& files,
                                               size_t inputId,
                                               size_t batchSize,
                                               const benchmark_app::InputInfo& inputInfo,
                                               std::string* filenames_used = nullptr) {
    size_t blob_size =
        std::accumulate(inputInfo.tensorShape.begin(), inputInfo.tensorShape.end(), 1, std::multiplies<int>());
    T* data = new T[blob_size];

    /** Collect images data ptrs **/
    std::vector<std::shared_ptr<uint8_t>> vreader;
    vreader.reserve(batchSize);

    for (size_t b = 0; b < batchSize; ++b) {
        auto inputIndex = (inputId + b) % files.size();
        if (filenames_used) {
            *filenames_used += (filenames_used->empty() ? "" : ", ") + files[inputIndex];
        }
        FormatReader::ReaderPtr reader(files[inputIndex].c_str());
        if (reader.get() == nullptr) {
            slog::warn << "Image " << files[inputIndex] << " cannot be read!" << slog::endl << slog::endl;
            continue;
        }

        /** Getting image data **/
        std::shared_ptr<uint8_t> imageData(reader->getData(inputInfo.width(), inputInfo.height()));
        if (imageData) {
            vreader.push_back(imageData);
        }
    }

    /** Fill input tensor with image. First b channel, then g and r channels **/
    const size_t numChannels = inputInfo.channels();
    const size_t width = inputInfo.width();
    const size_t height = inputInfo.height();
    /** Iterate over all input images **/
    for (size_t b = 0; b < batchSize; ++b) {
        /** Iterate over all width **/
        for (size_t w = 0; w < width; ++w) {
            /** Iterate over all height **/
            for (size_t h = 0; h < height; ++h) {
                /** Iterate over all channels **/
                for (size_t ch = 0; ch < numChannels; ++ch) {
                    /**          [images stride + channels stride + pixel id ] all in
                     * bytes            **/
                    size_t offset = b * numChannels * width * height +
                                    (((inputInfo.layout == "NCHW") || (inputInfo.layout == "CHW"))
                                         ? (ch * width * height + h * width + w)
                                         : (h * width * numChannels + w * numChannels + ch));
                    data[offset] =
                        (static_cast<T>(vreader.at(b).get()[h * width * numChannels + w * numChannels + ch]) -
                         static_cast<T>(inputInfo.mean[ch])) /
                        static_cast<T>(inputInfo.scale[ch]);
                }
            }
        }
    }

    InferenceEngine::TensorDesc tDesc(inputInfo.precision,
                                      inputInfo.tensorShape,
                                      getLayoutFromString(inputInfo.layout));
    auto blob =
        InferenceEngine::make_shared_blob<T>(tDesc,
                                             std::make_shared<SharedBlobAllocator<T>>(data, blob_size * sizeof(T)));
    blob->allocate();
    return blob;
}

template <typename T>
InferenceEngine::Blob::Ptr createBlobImInfo(const std::pair<size_t, size_t>& image_size,
                                            size_t batchSize,
                                            const benchmark_app::InputInfo& inputInfo) {
    size_t blob_size =
        std::accumulate(inputInfo.tensorShape.begin(), inputInfo.tensorShape.end(), 1, std::multiplies<int>());
    T* data = new T[blob_size];

    for (size_t b = 0; b < batchSize; b++) {
        size_t iminfoSize = blob_size / batchSize;
        for (size_t i = 0; i < iminfoSize; i++) {
            size_t index = b * iminfoSize + i;
            if (0 == i)
                data[index] = static_cast<T>(image_size.first);
            else if (1 == i)
                data[index] = static_cast<T>(image_size.second);
            else
                data[index] = 1;
        }
    }

    InferenceEngine::TensorDesc tDesc(inputInfo.precision,
                                      inputInfo.tensorShape,
                                      getLayoutFromString(inputInfo.layout));
    InferenceEngine::Blob::Ptr blob =
        InferenceEngine::make_shared_blob<T>(tDesc,
                                             std::make_shared<SharedBlobAllocator<T>>(data, blob_size * sizeof(T)));
    blob->allocate();
    return blob;
}

template <typename T>
InferenceEngine::Blob::Ptr createBlobFromBinary(const std::vector<std::string>& files,
                                                size_t inputId,
                                                size_t batchSize,
                                                const benchmark_app::InputInfo& inputInfo,
                                                std::string* filenames_used = nullptr) {
    size_t blob_size =
        std::accumulate(inputInfo.tensorShape.begin(), inputInfo.tensorShape.end(), 1, std::multiplies<int>());
    char* data = new char[blob_size * sizeof(T)];

    for (size_t b = 0; b < batchSize; ++b) {
        auto inputIndex = (inputId + b) % files.size();
        std::ifstream binaryFile(files[inputIndex], std::ios_base::binary | std::ios_base::ate);
        if (!binaryFile) {
            IE_THROW() << "Cannot open " << files[inputIndex];
        }

        auto fileSize = static_cast<std::size_t>(binaryFile.tellg());
        binaryFile.seekg(0, std::ios_base::beg);
        if (!binaryFile.good()) {
            IE_THROW() << "Can not read " << files[inputIndex];
        }
        auto inputSize = blob_size * sizeof(T) / batchSize;
        if (fileSize != inputSize) {
            IE_THROW() << "File " << files[inputIndex] << " contains " << std::to_string(fileSize)
                       << " bytes "
                          "but the network expects "
                       << std::to_string(inputSize);
        }
        binaryFile.read(&data[b * inputSize], inputSize);

        if (filenames_used) {
            *filenames_used += (filenames_used->empty() ? "" : ", ") + files[inputIndex];
        }
    }

    InferenceEngine::TensorDesc tDesc(inputInfo.precision,
                                      inputInfo.tensorShape,
                                      getLayoutFromString(inputInfo.layout));
    InferenceEngine::Blob::Ptr blob =
        InferenceEngine::make_shared_blob<T>(tDesc,
                                             std::make_shared<SharedBlobAllocator<T>>((T*)data, blob_size * sizeof(T)));
    blob->allocate();
    return blob;
}

template <typename T, typename T2>
InferenceEngine::Blob::Ptr createBlobRandom(const benchmark_app::InputInfo& inputInfo,
                                            T rand_min = std::numeric_limits<uint8_t>::min(),
                                            T rand_max = std::numeric_limits<uint8_t>::max()) {
    size_t blob_size =
        std::accumulate(inputInfo.tensorShape.begin(), inputInfo.tensorShape.end(), 1, std::multiplies<int>());
    T* data = new T[blob_size];

    std::mt19937 gen(0);
    uniformDistribution<T2> distribution(rand_min, rand_max);
    for (size_t i = 0; i < blob_size; i++) {
        data[i] = static_cast<T>(distribution(gen));
    }

    InferenceEngine::TensorDesc tDesc(inputInfo.precision,
                                      inputInfo.tensorShape,
                                      getLayoutFromString(inputInfo.layout));
    InferenceEngine::Blob::Ptr blob =
        InferenceEngine::make_shared_blob<T>(tDesc,
                                             std::make_shared<SharedBlobAllocator<T>>(data, blob_size * sizeof(T)));
    blob->allocate();
    return blob;
}

InferenceEngine::Blob::Ptr getImageBlob(const std::vector<std::string>& files,
                                        size_t inputId,
                                        size_t batchSize,
                                        const std::pair<std::string, benchmark_app::InputInfo>& inputInfo,
                                        std::string* filenames_used = nullptr) {
    auto precision = inputInfo.second.precision;
    if (precision == InferenceEngine::Precision::FP32) {
        return createBlobFromImage<float>(files, inputId, batchSize, inputInfo.second, filenames_used);
    } else if (precision == InferenceEngine::Precision::FP16) {
        return createBlobFromImage<short>(files, inputId, batchSize, inputInfo.second, filenames_used);
    } else if (precision == InferenceEngine::Precision::I32) {
        return createBlobFromImage<int32_t>(files, inputId, batchSize, inputInfo.second, filenames_used);
    } else if (precision == InferenceEngine::Precision::I64) {
        return createBlobFromImage<int64_t>(files, inputId, batchSize, inputInfo.second, filenames_used);
    } else if (precision == InferenceEngine::Precision::U8) {
        return createBlobFromImage<uint8_t>(files, inputId, batchSize, inputInfo.second, filenames_used);
    } else {
        IE_THROW() << "Input precision is not supported for " << inputInfo.first;
    }
}

InferenceEngine::Blob::Ptr getImInfoBlob(const std::pair<size_t, size_t>& image_size,
                                         size_t batchSize,
                                         const std::pair<std::string, benchmark_app::InputInfo>& inputInfo) {
    auto precision = inputInfo.second.precision;
    if (precision == InferenceEngine::Precision::FP32) {
        return createBlobImInfo<float>(image_size, batchSize, inputInfo.second);
    } else if (precision == InferenceEngine::Precision::FP16) {
        return createBlobImInfo<short>(image_size, batchSize, inputInfo.second);
    } else if (precision == InferenceEngine::Precision::I32) {
        return createBlobImInfo<int32_t>(image_size, batchSize, inputInfo.second);
    } else if (precision == InferenceEngine::Precision::I64) {
        return createBlobImInfo<int64_t>(image_size, batchSize, inputInfo.second);
    } else {
        IE_THROW() << "Input precision is not supported for " << inputInfo.first;
    }
}

InferenceEngine::Blob::Ptr getBinaryBlob(const std::vector<std::string>& files,
                                         size_t inputId,
                                         size_t batchSize,
                                         const std::pair<std::string, benchmark_app::InputInfo>& inputInfo,
                                         std::string* filenames_used = nullptr) {
    auto precision = inputInfo.second.precision;
    if (precision == InferenceEngine::Precision::FP32) {
        return createBlobFromBinary<float>(files, inputId, batchSize, inputInfo.second, filenames_used);
    } else if (precision == InferenceEngine::Precision::FP16) {
        return createBlobFromBinary<short>(files, inputId, batchSize, inputInfo.second, filenames_used);
    } else if (precision == InferenceEngine::Precision::I32) {
        return createBlobFromBinary<int32_t>(files, inputId, batchSize, inputInfo.second, filenames_used);
    } else if (precision == InferenceEngine::Precision::I64) {
        return createBlobFromBinary<int64_t>(files, inputId, batchSize, inputInfo.second, filenames_used);
    } else if ((precision == InferenceEngine::Precision::U8) || (precision == InferenceEngine::Precision::BOOL)) {
        return createBlobFromBinary<uint8_t>(files, inputId, batchSize, inputInfo.second, filenames_used);
    } else {
        IE_THROW() << "Input precision is not supported for " << inputInfo.first;
    }
}

InferenceEngine::Blob::Ptr getRandomBlob(const std::pair<std::string, benchmark_app::InputInfo>& inputInfo) {
    auto precision = inputInfo.second.precision;
    if (precision == InferenceEngine::Precision::FP32) {
        return createBlobRandom<float, float>(inputInfo.second);
    } else if (precision == InferenceEngine::Precision::FP16) {
        return createBlobRandom<short, short>(inputInfo.second);
    } else if (precision == InferenceEngine::Precision::I32) {
        return createBlobRandom<int32_t, int32_t>(inputInfo.second);
    } else if (precision == InferenceEngine::Precision::I64) {
        return createBlobRandom<int64_t, int64_t>(inputInfo.second);
    } else if (precision == InferenceEngine::Precision::U8) {
        // uniform_int_distribution<uint8_t> is not allowed in the C++17
        // standard and vs2017/19
        return createBlobRandom<uint8_t, uint32_t>(inputInfo.second);
    } else if (precision == InferenceEngine::Precision::I8) {
        // uniform_int_distribution<int8_t> is not allowed in the C++17 standard
        // and vs2017/19
        return createBlobRandom<int8_t, int32_t>(inputInfo.second);
    } else if (precision == InferenceEngine::Precision::U16) {
        return createBlobRandom<uint16_t, uint16_t>(inputInfo.second);
    } else if (precision == InferenceEngine::Precision::I16) {
        return createBlobRandom<int16_t, int16_t>(inputInfo.second);
    } else if (precision == InferenceEngine::Precision::BOOL) {
        return createBlobRandom<uint8_t, uint32_t>(inputInfo.second, 0, 1);
    } else {
        IE_THROW() << "Input precision is not supported for " << inputInfo.first;
    }
}

std::stringstream getTestInfoStreamHeader(benchmark_app::InputInfo& inputInfo) {
    std::stringstream strOut;
    strOut << "(" << inputInfo.layout << ", " << inputInfo.precision << ", " << getShapeString(inputInfo.tensorShape)
           << ", ";
    if (inputInfo.partialShape.is_dynamic()) {
        strOut << std::string("dyn:") << inputInfo.partialShape << "):\t";
    } else {
        strOut << "static):\t";
    }
    return strOut;
}

std::map<std::string, std::vector<InferenceEngine::Blob::Ptr>> getBlobs(
    std::map<std::string, std::vector<std::string>>& inputFiles,
    std::vector<benchmark_app::InputsInfo>& app_inputs_info) {
    std::map<std::string, std::vector<InferenceEngine::Blob::Ptr>> blobs;
    if (app_inputs_info.empty()) {
        throw std::logic_error("Inputs Info for network is empty!");
    }

    std::vector<std::pair<size_t, size_t>> net_input_im_sizes;
    for (auto& inputs_info : app_inputs_info) {
        for (auto& input : inputs_info) {
            if (input.second.isImage()) {
                net_input_im_sizes.push_back(std::make_pair(input.second.width(), input.second.height()));
            } else if (input.second.isImageInfo()) {
                // add image info name to the map<input_name, files> if it wasn't specified.
                // thus we make sure that this input will be filled later.
                if (!inputFiles.empty() && inputFiles.find(input.first) == inputFiles.end()) {
                    inputFiles[input.first] = {""};
                }
            }
        }
    }

    for (auto& files : inputFiles) {
        if (!files.first.empty() && app_inputs_info[0].find(files.first) == app_inputs_info[0].end()) {
            throw std::logic_error("Input name" + files.first +
                                   " used in -i parameter doesn't correspond any network's input");
        }

        std::string input_name = files.first.empty() ? app_inputs_info[0].begin()->first : files.first;
        auto input = app_inputs_info[0].at(input_name);
        if (input.isImage()) {
            files.second = filterFilesByExtensions(files.second, supported_image_extensions);
        } else if (input.isImageInfo() && net_input_im_sizes.size() == app_inputs_info.size()) {
            slog::info << "Input '" << input_name
                       << "' probably is image info. All files for this input will"
                          " be ignored."
                       << slog::endl;
            continue;
        } else {
            files.second = filterFilesByExtensions(files.second, supported_binary_extensions);
        }

        if (files.second.empty()) {
            throw std::logic_error("No suitable files for input found!");
        }

        size_t filesToBeUsed = 0;
        size_t shapesToBeUsed = 0;
        if (files.second.size() > app_inputs_info.size()) {
            shapesToBeUsed = app_inputs_info.size();
            filesToBeUsed = files.second.size() - files.second.size() % app_inputs_info.size();
            if (filesToBeUsed != files.second.size()) {
                slog::warn << "Number of files must be a multiple of the number of shapes for certain input. Only " +
                                  std::to_string(filesToBeUsed) + " files will be added."
                           << slog::endl;
            }
            while (files.second.size() != filesToBeUsed) {
                files.second.pop_back();
            }
        } else {
            shapesToBeUsed = app_inputs_info.size() - app_inputs_info.size() % files.second.size();
            filesToBeUsed = files.second.size();
            if (shapesToBeUsed != app_inputs_info.size()) {
                slog::warn << "Number of tensor shapes must be a multiple of the number of files. For input "
                           << files.first << " only " + std::to_string(shapesToBeUsed) + " files will be added."
                           << slog::endl;
            }
            while (app_inputs_info.size() != shapesToBeUsed) {
                app_inputs_info.pop_back();
                net_input_im_sizes.pop_back();
            }
        }
    }

    std::vector<std::map<std::string, std::string>> logOutput;
    // All inputs should process equal number of files, so for the case of N, 1, N number of files,
    // second input also should have N blobs cloned from 1 file
    size_t filesNum = 0;
    if (!inputFiles.empty()) {
        filesNum = std::max_element(inputFiles.begin(),
                                    inputFiles.end(),
                                    [](const std::pair<std::string, std::vector<std::string>>& a,
                                       const std::pair<std::string, std::vector<std::string>>& b) {
                                        return a.second.size() < b.second.size();
                                    })
                       ->second.size();
    }
    for (const auto& files : inputFiles) {
        std::string input_name = files.first.empty() ? app_inputs_info[0].begin()->first : files.first;
        size_t n_shape = 0, m_file = 0;
        while (n_shape < app_inputs_info.size() || m_file < filesNum) {
            auto app_info = app_inputs_info[n_shape % app_inputs_info.size()].at(input_name);
            auto precision = app_info.precision;
            size_t inputId = m_file % files.second.size();
            size_t batchSize = app_info.batch();

            std::string blob_src_info;
            if (app_info.isImage()) {
                // Fill with Images
                blobs[input_name].push_back(
                    getImageBlob(files.second, inputId, batchSize, {input_name, app_info}, &blob_src_info));
            } else if (app_info.isImageInfo() && net_input_im_sizes.size() == app_inputs_info.size()) {
                // Most likely it is image info: fill with image information
                auto image_size = net_input_im_sizes.at(n_shape % app_inputs_info.size());
                blob_src_info =
                    "Image size blob " + std::to_string(image_size.first) + " x " + std::to_string(image_size.second);
                blobs[input_name].push_back(getImInfoBlob(image_size, batchSize, {input_name, app_info}));
            } else {
                // Fill with binary files
                blobs[input_name].push_back(
                    getBinaryBlob(files.second, inputId, batchSize, {input_name, app_info}, &blob_src_info));
            }

            // Preparing info
            std::stringstream strOut = getTestInfoStreamHeader(app_info);
            strOut << blob_src_info;
            if (n_shape >= logOutput.size()) {
                logOutput.resize(n_shape + 1);
            }
            logOutput[n_shape][input_name] += strOut.str();

            ++n_shape;
            m_file += app_info.batch();
        }
    }

    if (inputFiles.empty()) {
        slog::warn << "No input files were given: all inputs will be filled with "
                      "random values!"
                   << slog::endl;

        size_t i = 0;
        size_t n_shape = 0;
        logOutput.resize(app_inputs_info.size());

        for (auto& input_info : app_inputs_info) {
            for (auto& input : input_info) {
                // Preparing info
                std::stringstream strOut = getTestInfoStreamHeader(input.second);
                size_t batchSize = input.second.batch();
                if (input.second.isImageInfo() && net_input_im_sizes.size() == app_inputs_info.size()) {
                    // Most likely it is image info: fill with image information
                    auto image_size = net_input_im_sizes.at(i);
                    strOut << "Image size blob '" << image_size.first << "x" << image_size.second;
                    blobs[input.first].push_back(getImInfoBlob(image_size, batchSize, input));
                    ++i;
                } else {
                    // Fill random
                    strOut << "random (" << std::string((input.second.isImage() ? "image" : "binary data"))
                           << " is expected)";
                    blobs[input.first].push_back(getRandomBlob(input));
                }

                logOutput[n_shape][input.first] += strOut.str();
            }
            n_shape++;
        }
    }

    for (int i = 0; i < logOutput.size(); i++) {
        slog::info << "Test Config " << i << slog::endl;
        auto maxNameWidth = std::max_element(logOutput[i].begin(),
                                             logOutput[i].end(),
                                             [](const std::pair<std::string, std::string>& a,
                                                const std::pair<std::string, std::string>& b) {
                                                 return a.first.size() < b.first.size();
                                             })
                                ->first.size();
        for (auto inputLog : logOutput[i]) {
            slog::info << std::left << std::setw(maxNameWidth + 2) << inputLog.first << inputLog.second << slog::endl;
        }
    }

    return blobs;
}
