// Copyright (C) 2019-2020 Siodb GmbH. All rights reserved.
// Use of this source code is governed by a license that can be found
// in the LICENSE file.

#include "Client.h"
#include "internal/ClientInternal.h"

// Common project headers
#include <siodb/common/config/SiodbDefs.h>
#include <siodb/common/crt_ext/ct_string.h>
#include <siodb/common/crypto/DigitalSignatureKey.h>
#include <siodb/common/data/RawDateTime.h>
#include <siodb/common/io/FileIO.h>
#include <siodb/common/io/StreamFormatGuard.h>
#include <siodb/common/protobuf/ProtobufMessageIO.h>
#include <siodb/common/protobuf/RawDateTimeIO.h>
#include <siodb/common/stl_ext/utility_ext.h>
#include <siodb/common/utils/Bitmask.h>
#include <siodb/common/utils/StringBuilder.h>
#include <siodb/common/utils/SystemError.h>

// Protobuf message headers
#include <siodb/common/proto/ClientProtocol.pb.h>

// CRT headers
#include <cstdio>
#include <cstring>

// STL headers
#include <array>
#include <iomanip>
#include <sstream>

// Boost headers
#include <boost/endian/conversion.hpp>

// Protobuf headers
#include <google/protobuf/io/zero_copy_stream_impl.h>

// utf8cpp headers
#include <utf8cpp/utf8.h>

void executeCommandOnServer(std::uint64_t requestId, std::string&& commandText,
        siodb::io::IoBase& connectionIo, std::ostream& os, bool stopOnError)
{
    auto startTime = std::chrono::steady_clock::now();
    // Send command to server as protobuf message
    siodb::client_protocol::Command command;
    command.set_request_id(requestId);
    command.set_text(std::move(commandText));
    siodb::protobuf::writeMessage(
            siodb::protobuf::ProtocolMessageType::kCommand, command, connectionIo);

    std::size_t responseId = 0, responseCount = 0;
    do {
        // Read server response
        // Allow EINTR to cause I/O error when exit signal detected.
        const siodb::utils::DefaultErrorCodeChecker errorCodeChecker;

        siodb::client_protocol::ServerResponse response;
        siodb::protobuf::CustomProtobufInputStream input(connectionIo, errorCodeChecker);
        siodb::protobuf::readMessage(
                siodb::protobuf::ProtocolMessageType::kServerResponse, response, input);

#ifdef _DEBUG
        std::cerr << "\ndebug: ==================================================================="
                     "====\n"
                  << "debug: Expecting response: requestId=" << requestId
                  << " responseId=" << responseId
                  << "\ndebug: Received response: requestId=" << response.request_id()
                  << " responseId=" << response.response_id()
                  << "\ndebug: ==================================================================="
                     "====\n"
                  << std::flush;
#endif

        // Check request ID
        if (response.request_id() != requestId) {
            std::ostringstream err;
            err << "Wrong request ID in the server response: expecting " << requestId
                << ", but received " << response.request_id();
            throw std::runtime_error(err.str());
        }

        // Check reponse ID
        if (response.response_id() != responseId) {
            std::ostringstream err;
            err << "Wrong response ID in the server response: expecting " << responseId
                << ", but received " << response.response_id();
            throw std::runtime_error(err.str());
        }

        // Capture response count
        if (responseId == 0) {
            responseCount = response.response_count();
            if (responseCount == 0) responseCount = 1;
            // os << "Number of responses:" << responseCount << std::endl;
        } else {
            // Print extra separator lines between responses.
            os << "\n\n";
        }

        // Print "freetext" messages
        const int freeTextMessageCount = response.freetext_message_size();
        if (freeTextMessageCount > 0) {
            os << '\n';
            for (int i = 0; i < freeTextMessageCount; ++i) {
                os << "Server: " << response.freetext_message(i) << '\n';
            }
            os << std::endl;  // newline + flush stream at this point
        }

        bool sqlErrorOccurred = false;
        // Print messages
        const int messageCount = response.message_size();
        if (messageCount > 0) {
            os << std::endl;
            for (int i = 0; i < messageCount; ++i) {
                const auto& message = response.message(i);
                os << "Status " << message.status_code() << ": " << message.text() << '\n';
                sqlErrorOccurred |= message.status_code() != 0;
            }
            os << std::endl;  // newline + flush stream at this point
        }

        if (sqlErrorOccurred) {
            const auto endTime = std::chrono::steady_clock::now();
            const auto elapsed =
                    std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
            os << "Command execution time: " << elapsed.count() << " ms." << std::endl;
            startTime = endTime;
            ++responseId;
            if (stopOnError) throw std::runtime_error("SQL error");
            continue;
        }

        // Check dataset presence
        const int columnCount = response.column_description_size();
        if (columnCount > 0) {
            // Create CodedInputStream only if row data is available to read
            // otherwise codedInput constructor will be stucked on waiting on buffering data
            google::protobuf::io::CodedInputStream codedInput(&input);

            struct ColumnPrintInfo {
                siodb::ColumnDataType type;
                std::size_t width;
            };

            // Compute column widths
            std::vector<ColumnPrintInfo> columnPrintInfo;
            columnPrintInfo.reserve(columnCount);

            bool nullAllowed = false;
            for (int i = 0; i < columnCount; ++i) {
                const auto& column = response.column_description(i);
                const auto columnDataWidth =
                        getColumnDataWidth(column.type(), column.name().length());

                columnPrintInfo.push_back({column.type(), columnDataWidth});
                nullAllowed |= column.is_null();
            }

            // Print column names
            for (int i = 0; i < columnCount; ++i) {
                if (i > 0) os << ' ';  // one space
                const auto& column = response.column_description(i);
                os << column.name();
                const auto spaceCount = columnPrintInfo[i].width - column.name().length();
                std::fill_n(std::ostream_iterator<char>(os), spaceCount, ' ');
            }
            os << '\n';

            // Print dashes
            for (int i = 0; i < columnCount; ++i) {
                const auto dashCount = columnPrintInfo[i].width;
                for (std::size_t j = 0; j < dashCount; ++j) {
                    os << '-';
                }
                os << ' ';  // one space
            }
            os << std::endl;  // newline + flush stream at this point

            // Receive and print column data
            std::uint64_t row = 0;
            while (true) {
                std::uint64_t rowLength = 0;
                if (!codedInput.ReadVarint64(&rowLength)) {
                    std::ostringstream err;
                    err << "Can't read from server: " << std::strerror(input.GetErrno());
                    throw std::system_error(input.GetErrno(), std::generic_category(), err.str());
                }
                if (rowLength == 0) break;

                siodb::utils::Bitmask nullBitmask;
                // Server is going to provide next row, read it.
                if (nullAllowed) {
                    nullBitmask.resize(columnCount, false);
                    if (!codedInput.ReadRaw(nullBitmask.getData(), nullBitmask.getByteSize())) {
                        std::ostringstream err;
                        err << "Can't read from server: " << std::strerror(input.GetErrno());
                        throw std::system_error(
                                input.GetErrno(), std::generic_category(), err.str());
                    }
                }

                for (int col = 0; col < columnCount; ++col) {
                    if (col > 0) {
                        os << ' ';  // one space
                    }

                    auto columnType = columnPrintInfo[col].type;
                    if (nullAllowed && nullBitmask.getBit(col)) {
                        columnType = siodb::COLUMN_DATA_TYPE_UNKNOWN;
                        columnPrintInfo[col].width = kNullDataWidth;
                    }

                    if (!receiveAndPrintColumnData(
                                codedInput, columnType, columnPrintInfo[col].width, os)) {
                        std::ostringstream err;
                        err << "Can't read from server: " << std::strerror(input.GetErrno());
                        throw std::system_error(
                                input.GetErrno(), std::generic_category(), err.str());
                    }
                }
                os << '\n';
                ++row;
            }
            // Print number of rows
            os << '\n' << row << " rows.\n" << std::flush;
        } else {
            if (response.has_affected_row_count()) {
                os << response.affected_row_count() << " rows affected" << std::endl;
            }
        }

        const auto endTime = std::chrono::steady_clock::now();
        const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        os << "Command execution time: " << elapsed.count() << " ms." << std::endl;
        startTime = endTime;

        // Increment response ID.
        ++responseId;
    } while (responseId < responseCount);
}

void authenticate(const std::string& identityKey, const std::string& userName,
        siodb::io::IoBase& connectionIo)
{
    siodb::client_protocol::BeginSessionRequest beginSessionRequest;
    beginSessionRequest.set_user_name(userName);
    siodb::protobuf::writeMessage(siodb::protobuf::ProtocolMessageType::kClientBeginSessionRequest,
            beginSessionRequest, connectionIo);

    siodb::client_protocol::BeginSessionResponse beginSessionResponse;
    const siodb::utils::DefaultErrorCodeChecker errorCodeChecker;
    siodb::protobuf::CustomProtobufInputStream input(connectionIo, errorCodeChecker);
    siodb::protobuf::readMessage(siodb::protobuf::ProtocolMessageType::kClientBeginSessionResponse,
            beginSessionResponse, input);

    if (!beginSessionResponse.session_started()) {
        if (beginSessionResponse.has_message()) {
            throw std::runtime_error(
                    siodb::utils::StringBuilder()
                    << "Begin session error: " << beginSessionResponse.message().status_code()
                    << " " << beginSessionResponse.message().text());
        }
        throw std::runtime_error("Begin session unknown error");
    }

    const auto& challenge = beginSessionResponse.challenge();
    siodb::crypto::DigitalSignatureKey key;
    key.parseFromString(identityKey);
    auto signature = key.signMessage(challenge);

    siodb::client_protocol::ClientAuthenticationRequest authRequest;
    authRequest.set_signature(std::move(signature));
    siodb::protobuf::writeMessage(
            siodb::protobuf::ProtocolMessageType::kClientAuthenticationRequest, authRequest,
            connectionIo);

    siodb::client_protocol::ClientAuthenticationResponse authResponse;
    siodb::protobuf::readMessage(
            siodb::protobuf::ProtocolMessageType::kClientAuthenticationResponse, authResponse,
            input);

    if (authResponse.has_message()) {
        std::cerr << "Authentication error: " << authResponse.message().status_code() << " "
                  << authResponse.message().text();
    }

    if (!authResponse.authenticated()) throw std::runtime_error("User authentication error");
}

namespace {

using DataTypeWidthArray =
        std::array<std::size_t, static_cast<size_t>(siodb::COLUMN_DATA_TYPE_MAX)>;

constexpr DataTypeWidthArray kDefaultDataWidths {
        kBoolDefaultDataWidth,  // COLUMN_DATA_TYPE_BOOL = 0,
        kInt8DefaultDataWidth,  // COLUMN_DATA_TYPE_INT8 = 1,
        kUInt8DefaultDataWidth,  // COLUMN_DATA_TYPE_UINT8 = 2,
        kInt16DefaultDataWidth,  // COLUMN_DATA_TYPE_INT16 = 3,
        kUInt16DefaultDataWidth,  // COLUMN_DATA_TYPE_UINT16 = 4,
        kInt32DefaultDataWidth,  // COLUMN_DATA_TYPE_INT32 = 5,
        kUInt32DefaultDataWidth,  // COLUMN_DATA_TYPE_UINT32 = 6,
        kInt64DefaultDataWidth,  // COLUMN_DATA_TYPE_INT64 = 7,
        kUInt64DefaultDataWidth,  // COLUMN_DATA_TYPE_UINT64 = 8,
        kFloatDefaultDataWidth,  // COLUMN_DATA_TYPE_FLOAT = 9,
        kDoubleDefaultDataWidth,  // COLUMN_DATA_TYPE_DOUBLE = 10,
        kTextDefaultDataWidth,  // COLUMN_DATA_TYPE_TEXT = 11,
        kNTextDefaultDataWidth,  // COLUMN_DATA_TYPE_NTEXT = 12,
        kBinaryDefaultDataWidth,  // COLUMN_DATA_TYPE_BINARY = 13,
        kDateDefaultDataWidth,  // COLUMN_DATA_TYPE_DATE = 14,
        kTimeDefaultDataWidth,  // COLUMN_DATA_TYPE_TIME = 15,
        kTimeWithTzDefaultDataWidth,  // COLUMN_DATA_TYPE_TIME_WITH_TZ = 16,
        kTimestampDefaultDataWidth,  // COLUMN_DATA_TYPE_TIMESTAMP = 17,
        kTimestampWithTzDefaultDataWidth,  // COLUMN_DATA_TYPE_TIMESTAMP_WITH_TZ = 18,
        kDateIntervalWithTzDefaultDataWidth,  // COLUMN_DATA_TYPE_DATE_INTERVAL = 19,
        kTimeIntervalDefaultDataWidth,  // COLUMN_DATA_TYPE_TIME_INTERVAL = 20,
        kStructDefaultDataWidth,  // COLUMN_DATA_TYPE_STRUCT = 21,
        kXmlDefaultDataWidth,  // COLUMN_DATA_TYPE_XML = 22,
        kJsonDefaultDataWidth,  // COLUMN_DATA_TYPE_JSON = 23,
        kUuidDefaultDataWidth  // COLUMN_DATA_TYPE_UUID = 24,
};

static_assert(kDefaultDataWidths[siodb::COLUMN_DATA_TYPE_DOUBLE] == kDoubleDefaultDataWidth);

const char* kInvalidDayOfWeekShortName = "???";
const char* kInvalidMonthShortName = "???";
const char* kAm = "AM";
const char* kPm = "PM";
const char* kUndefinedAmPm = "??";

constexpr const char* kBlobDisplayPrefix = "0x";
constexpr auto kBlobDisplayPrefixLength = ct_strlen(kBlobDisplayPrefix);
static_assert(
        kBlobDisplayPrefixLength < kBinaryDefaultDataWidth / 2, "kBlobDisplayPrefix is too long");

constexpr const char* kLobDisplaySuffix = "...";
constexpr auto kLobDisplaySuffixLength = ct_strlen(kLobDisplaySuffix);
static_assert(kLobDisplaySuffixLength < kTextDefaultDataWidth, "kLobDisplaySuffix is too long");
static_assert(kLobDisplaySuffixLength < kBinaryDefaultDataWidth - kBlobDisplayPrefixLength,
        "kLobDisplaySuffix is too long");

const auto kBlobPrintableLengthDecreaseForLobSuffix =
        (kLobDisplaySuffixLength / 2) + (kLobDisplaySuffixLength % 2);

std::size_t getColumnDataWidth(siodb::ColumnDataType type, std::size_t nameLength)
{
    return (type >= 0 && type < siodb::COLUMN_DATA_TYPE_MAX)
                   ? std::max(kDefaultDataWidths[type], nameLength)
                   : nameLength;
}

bool receiveAndPrintColumnData(google::protobuf::io::CodedInputStream& is,
        siodb::ColumnDataType type, std::size_t width, std::ostream& os)
{
    siodb::io::StreamFormatGuard formatGuard(os);
    switch (type) {
        case siodb::COLUMN_DATA_TYPE_UNKNOWN: {
            os.width(width);
            os << "null";
            return true;
        }

        case siodb::COLUMN_DATA_TYPE_BOOL: {
            std::uint8_t data = 0;
            if (is.ReadRaw(&data, sizeof(data))) {
                os.width(width);
                os << std::boolalpha << (data != 0);
                return true;
            }
            return false;
        }

        case siodb::COLUMN_DATA_TYPE_INT8: {
            std::int8_t data = 0;
            if (is.ReadRaw(&data, sizeof(data))) {
                os.width(width);
                os << static_cast<int>(data);
                return true;
            }
            return false;
        }

        case siodb::COLUMN_DATA_TYPE_UINT8: {
            std::uint8_t data = 0;
            if (is.ReadRaw(&data, sizeof(data))) {
                os.width(width);
                os << static_cast<int>(data);
                return true;
            }
            return false;
        }

        case siodb::COLUMN_DATA_TYPE_INT16: {
            std::int16_t data = 0;
            if (is.ReadRaw(&data, sizeof(data))) {
                boost::endian::little_to_native_inplace(data);
                os.width(width);
                os << data;
                return true;
            }
            return false;
        }

        case siodb::COLUMN_DATA_TYPE_UINT16: {
            std::uint16_t data = 0;
            if (is.ReadRaw(&data, sizeof(data))) {
                boost::endian::little_to_native_inplace(data);
                os.width(width);
                os << data;
                return true;
            }
            return false;
        }

        case siodb::COLUMN_DATA_TYPE_INT32: {
            std::int32_t data = 0;
            if (is.ReadVarint32(reinterpret_cast<std::uint32_t*>(&data))) {
                os.width(width);
                os << data;
                return true;
            }
            return false;
        }

        case siodb::COLUMN_DATA_TYPE_UINT32: {
            std::uint32_t data = 0;
            if (is.ReadVarint32(&data)) {
                os.width(width);
                os << data;
                return true;
            }
            return false;
        }

        case siodb::COLUMN_DATA_TYPE_INT64: {
            std::int64_t data = 0;
            if (is.ReadVarint64(reinterpret_cast<std::uint64_t*>(&data))) {
                os.width(width);
                os << data;
                return true;
            }
            return false;
        }

        case siodb::COLUMN_DATA_TYPE_UINT64: {
            std::uint64_t data = 0;
            if (is.ReadVarint64(&data)) {
                os.width(width);
                os << data;
                return true;
            }
            return false;
        }

        case siodb::COLUMN_DATA_TYPE_FLOAT: {
            float data = 0.0f;
            if (is.ReadLittleEndian32(reinterpret_cast<std::uint32_t*>(&data))) {
                os.width(width);
                os.precision(width);
                os << data;
                return true;
            }
            return false;
        }

        case siodb::COLUMN_DATA_TYPE_DOUBLE: {
            double data = 0.0;
            if (is.ReadLittleEndian64(reinterpret_cast<std::uint64_t*>(&data))) {
                os.width(width);
                os.precision(width);
                os << data;
                return true;
            }
            return false;
        }

        case siodb::COLUMN_DATA_TYPE_TEXT: {
            std::uint32_t clobLength = 0;
            // Read length
            if (!is.ReadVarint32(&clobLength)) return false;

            // Read sample
            char buffer[kTextDefaultDataWidth * 4 + 1];
            const auto sampleLength = std::min(static_cast<size_t>(clobLength), sizeof(buffer) - 1);
            if (!is.ReadRaw(buffer, sampleLength)) return false;

            // Count valid codepoints up to allowed limit
            // and convert control characters into escape sequences
            std::uint32_t codePointsBuffer[kTextDefaultDataWidth];
            std::uint32_t* currentCodePoint = &codePointsBuffer[0];
            std::size_t numberOfValidCodePoints = 0;
            const char* bufferEnd = &buffer[sampleLength];
            const char* currentChar = &buffer[0];
            {
                try {
                    bool scanning = true;
                    while (scanning && numberOfValidCodePoints < kTextDefaultDataWidth) {
                        const auto codePoint = utf8::next(currentChar, bufferEnd);
                        switch (codePoint) {
                            case '\a': {
                                if (numberOfValidCodePoints + 2 > kTextDefaultDataWidth) {
                                    --currentChar;
                                    scanning = false;
                                    break;
                                }
                                *currentCodePoint++ = '\\';
                                *currentCodePoint++ = 'a';
                                numberOfValidCodePoints += 2;
                                break;
                            }
                            case '\b': {
                                if (numberOfValidCodePoints + 2 > kTextDefaultDataWidth) {
                                    --currentChar;
                                    scanning = false;
                                    break;
                                }
                                *currentCodePoint++ = '\\';
                                *currentCodePoint++ = 'b';
                                numberOfValidCodePoints += 2;
                                break;
                            }
                            case '\f': {
                                if (numberOfValidCodePoints + 2 > kTextDefaultDataWidth) {
                                    --currentChar;
                                    scanning = false;
                                    break;
                                }
                                *currentCodePoint++ = '\\';
                                *currentCodePoint++ = 'f';
                                numberOfValidCodePoints += 2;
                                break;
                            }
                            case '\n': {
                                if (numberOfValidCodePoints + 2 > kTextDefaultDataWidth) {
                                    --currentChar;
                                    scanning = false;
                                    break;
                                }
                                *currentCodePoint++ = '\\';
                                *currentCodePoint++ = 'n';
                                numberOfValidCodePoints += 2;
                                break;
                            }
                            case '\r': {
                                if (numberOfValidCodePoints + 2 > kTextDefaultDataWidth) {
                                    --currentChar;
                                    scanning = false;
                                    break;
                                }
                                *currentCodePoint++ = '\\';
                                *currentCodePoint++ = 'r';
                                numberOfValidCodePoints += 2;
                                break;
                            }
                            case '\t': {
                                if (numberOfValidCodePoints + 2 > kTextDefaultDataWidth) {
                                    --currentChar;
                                    scanning = false;
                                    break;
                                }
                                *currentCodePoint++ = '\\';
                                *currentCodePoint++ = 't';
                                numberOfValidCodePoints += 2;
                                break;
                            }
                            case '\v': {
                                if (numberOfValidCodePoints + 2 > kTextDefaultDataWidth) {
                                    --currentChar;
                                    scanning = false;
                                    break;
                                }
                                *currentCodePoint++ = '\\';
                                *currentCodePoint++ = 'v';
                                numberOfValidCodePoints += 2;
                                break;
                            }
                            case 0x1B: {
                                if (numberOfValidCodePoints + 4 > kTextDefaultDataWidth) {
                                    --currentChar;
                                    scanning = false;
                                    break;
                                }
                                *currentCodePoint++ = '\\';
                                *currentCodePoint++ = 'E';
                                *currentCodePoint++ = 'S';
                                *currentCodePoint++ = 'C';
                                numberOfValidCodePoints += 4;
                                break;
                            }
                            case 0x9B: {
                                if (numberOfValidCodePoints + 4 > kTextDefaultDataWidth) {
                                    --currentChar;
                                    scanning = false;
                                    break;
                                }
                                *currentCodePoint++ = '\\';
                                *currentCodePoint++ = 'C';
                                *currentCodePoint++ = 'S';
                                *currentCodePoint++ = 'I';
                                numberOfValidCodePoints += 4;
                                break;
                            }
                            default: {
                                *currentCodePoint++ = codePoint;
                                ++numberOfValidCodePoints;
                                break;
                            }
                        }
                    }
                } catch (utf8::exception& ex) {
                    // just ignore
                }
            }  // scan text

            // Calculate printable width
            bool printSuffix = false;
            std::size_t printableWidth = numberOfValidCodePoints;
            if (currentChar == bufferEnd) {
                if (printableWidth > kTextDefaultDataWidth) {
                    printSuffix = true;
                    printableWidth = kTextDefaultDataWidth - kLobDisplaySuffixLength;
                }
            } else {
                printSuffix = true;
                if (printableWidth + kLobDisplaySuffixLength > kTextDefaultDataWidth)
                    printableWidth = kTextDefaultDataWidth - kLobDisplaySuffixLength;
            }

            // Convert allowed number of code points back to UTF-8
            auto textEnd =
                    utf8::utf32to8(&codePointsBuffer[0], &codePointsBuffer[printableWidth], buffer);
            *textEnd = '\0';

            // Fill up to required length
            auto printedWidth = printableWidth;
            if (printSuffix) printedWidth += kLobDisplaySuffixLength;
            if (printedWidth < kTextDefaultDataWidth) {
                os.width(kTextDefaultDataWidth - printedWidth);
                os << " ";  // IMPORTANT: must be string, not single character
            }

            // Print string and leftover
            os.width(0);
            os << buffer;
            if (printSuffix) os << kLobDisplaySuffix;

            // Read remaining data
            if (sampleLength < clobLength) {
                std::vector<char> buffer2(kLobReadBufferSize);
                auto remaining = clobLength - sampleLength;
                while (remaining > 0) {
                    const auto readSize = std::min(remaining, buffer2.size());
                    if (!is.ReadRaw(buffer2.data(), readSize)) return false;
                    remaining -= readSize;
                }
            }
            return true;
        }

        case siodb::COLUMN_DATA_TYPE_BINARY: {
            std::uint32_t blobLength = 0;
            if (!is.ReadVarint32(&blobLength)) return false;

            // Read sample
            std::uint8_t buffer[(kBinaryDefaultDataWidth - kBlobDisplayPrefixLength) / 2];
            const auto sampleLength =
                    std::min(static_cast<std::size_t>(blobLength), sizeof(buffer));
            if (blobLength > 0) {
                if (!is.ReadRaw(buffer, sampleLength)) return false;
            }

            // Determine printable length
            bool printSuffix = false;
            std::size_t printableLength = sampleLength;
            if (printableLength < blobLength) {
                printSuffix = true;
                printableLength -= kBlobPrintableLengthDecreaseForLobSuffix;
            }

            // Fill up to required length
            auto printedWidth = printableLength * 2 + kBlobDisplayPrefixLength;
            if (printSuffix) printedWidth += kLobDisplaySuffixLength;
            if (printedWidth < kBinaryDefaultDataWidth) {
                os.width(kBinaryDefaultDataWidth - printedWidth - 1);
                os << " ";  // IMPORTANT: must be string, not single character
            }

            // Print sample
            os.width(0);
            os << kBlobDisplayPrefix;
            os << std::setfill('0') << std::hex;
            for (std::size_t i = 0; i < printableLength; ++i) {
                const std::uint16_t v = buffer[i];
                os << std::setw(2) << v;
            }
            os.fill(0);
            if (printSuffix) {
                os.width(0);
                os << kLobDisplaySuffix;
            }

            // Read remaining data
            if (sampleLength < blobLength) {
                std::vector<char> buffer2(kLobReadBufferSize);
                auto remaining = blobLength - sampleLength;
                while (remaining > 0) {
                    const auto readSize = std::min(remaining, buffer2.size());
                    if (!is.ReadRaw(buffer2.data(), readSize)) return false;
                    remaining -= readSize;
                }
            }
            return true;
        }

        case siodb::COLUMN_DATA_TYPE_TIMESTAMP: {
            // Read value
            siodb::RawDateTime dateTime;
            if (!siodb::protobuf::readRawDateTime(is, dateTime)) return false;

            // Print value
            char buffer[kTimestampDefaultDataWidth * 2];
            const auto dayOfWeek = siodb::getDayOfWeekShortName(dateTime.m_datePart.m_dayOfWeek);
            const auto month = siodb::getDayMonthShortName(dateTime.m_datePart.m_month);
            std::pair<unsigned, bool> hours;
            const auto hoursValid = siodb::convertHours24To12(dateTime.m_timePart.m_hours, hours);
            std::snprintf(buffer, sizeof(buffer), "%.3s %.3s %02u %d %02u:%02u:%02u.%09u %.2s",
                    dayOfWeek ? dayOfWeek : kInvalidDayOfWeekShortName,
                    month ? month : kInvalidMonthShortName, dateTime.m_datePart.m_dayOfMonth + 1,
                    dateTime.m_datePart.m_year,
                    hoursValid ? hours.first : dateTime.m_timePart.m_hours,
                    dateTime.m_timePart.m_minutes, dateTime.m_timePart.m_seconds,
                    dateTime.m_timePart.m_nanos,
                    hoursValid ? (hours.second ? kPm : kAm) : kUndefinedAmPm);
            os.width(width);
            os << buffer;
            return true;
        }

        default: {
            std::ostringstream err;
            err << "Unsupported column data type " << static_cast<int>(type);
            throw std::invalid_argument(err.str());
        }
    }
}

}  // anonymous namespace
