// Copyright (C) 2019-2020 Siodb GmbH. All rights reserved.
// Use of this source code is governed by a license that can be found
// in the LICENSE file.

#include "InstanceOptions.h"

// Project headers
#include "DatabaseInstance.h"
#include "../net/NetConstants.h"
#include "../stl_wrap/filesystem_wrapper.h"

// Internal headers
#include "internal/DatabaseOptionsInternal.h"

// CRT headers
#include <cstring>

// STL headers
#include <array>
#include <unordered_set>

// Boost headers
#include <boost/algorithm/string.hpp>

namespace siodb::config {

namespace {
const std::array<const char*, static_cast<std::size_t>(boost::log::trivial::fatal) + 1>
        logLevelNames {"trace", "debug", "info", "warning", "error", "fatal"};

inline auto constructOptionPath(const std::string& str)
{
    return boost::property_tree::ptree::path_type(str, ';');
}

struct BoolTranslator {
    typedef std::string internal_type;
    typedef int external_type;

    boost::optional<bool> get_value(const std::string& option)
    {
        if ((::strcasecmp(option.c_str(), "true") == 0)
                || (::strcasecmp(option.c_str(), "yes") == 0))
            return boost::make_optional(true);
        else if ((::strcasecmp(option.c_str(), "false") == 0)
                 || (::strcasecmp(option.c_str(), "no") == 0))
            return boost::make_optional(false);
        else
            return boost::none;
    }
};

}  // namespace

std::string InstanceOptions::getExecutableDir() const
{
    const fs::path path(m_generalOptions.m_executablePath);
    return path.parent_path().native();
}

void InstanceOptions::load(const std::string& instanceName)
{
    const auto config = readConfiguration(instanceName);
    InstanceOptions tmpOptions;

    // Instance options

    tmpOptions.m_generalOptions.m_name = instanceName;

    // Parse IPv4 port number
    tmpOptions.m_generalOptions.m_ipv4port =
            config.get<int>(constructOptionPath(kGeneralOptionIpv4Port), kDefaultIpv4PortNumber);
    if (tmpOptions.m_generalOptions.m_ipv4port != 0
            && (tmpOptions.m_generalOptions.m_ipv4port < kMinPortNumber
                    || tmpOptions.m_generalOptions.m_ipv4port > kMaxPortNumber))
        throw InvalidConfigurationOptionError("Invalid IPv4 server port number");

    // Parse IPv6 port number
    tmpOptions.m_generalOptions.m_ipv6port =
            config.get<int>(constructOptionPath(kGeneralOptionIpv6Port), kDefaultIpv6PortNumber);
    if (tmpOptions.m_generalOptions.m_ipv6port != 0
            && (tmpOptions.m_generalOptions.m_ipv6port < kMinPortNumber
                    || tmpOptions.m_generalOptions.m_ipv6port > kMaxPortNumber))
        throw InvalidConfigurationOptionError("Invalid IPv6 server port number");

    if (tmpOptions.m_generalOptions.m_ipv6port == 0 && tmpOptions.m_generalOptions.m_ipv4port == 0)
        throw InvalidConfigurationOptionError("Both IPv4 and IPv6 are disabled");

    // Parse data directory
    tmpOptions.m_generalOptions.m_dataDirectory = boost::trim_copy(
            config.get<std::string>(constructOptionPath(kGeneralOptionDataDirectory), ""));
    while (!tmpOptions.m_generalOptions.m_dataDirectory.empty()
            && tmpOptions.m_generalOptions.m_dataDirectory.back() == '/') {
        tmpOptions.m_generalOptions.m_dataDirectory.erase(
                tmpOptions.m_generalOptions.m_dataDirectory.length() - 1);
    }
    if (tmpOptions.m_generalOptions.m_dataDirectory.empty())
        throw InvalidConfigurationOptionError("Data directory not specified or empty");

    // Parse admin connection listener backlog
    const auto adminConnectionListenerBacklog =
            config.get<int>(constructOptionPath(kGeneralOptionAdminConnectionListenerBacklog),
                    kDefaultAdminConnectionListenerBacklog);
    if (adminConnectionListenerBacklog < 1
            || adminConnectionListenerBacklog > kMaxAdminConnectionListenerBacklog) {
        throw InvalidConfigurationOptionError(
                "Admin connection listener backlog value is out of range");
    }
    tmpOptions.m_generalOptions.m_adminConnectionListenerBacklog = adminConnectionListenerBacklog;

    // Parse max number of admin connections
    const auto maxAdminConnections = config.get<unsigned>(
            constructOptionPath(kGeneralOptionMaxAdminConnections), kDefaultMaxAdminConnections);
    if (maxAdminConnections < 1 || maxAdminConnections > kMaxMaxAdminConnections) {
        throw InvalidConfigurationOptionError("Max. number of admin connections is out of range");
    }
    tmpOptions.m_generalOptions.m_maxAdminConnections = maxAdminConnections;

    // Parse user connection listener backlog
    const auto userConnectionListenerBacklog =
            config.get<int>(constructOptionPath(kGeneralOptionUserConnectionListenerBacklog),
                    kDefaultUserConnectionListenerBacklog);
    if (userConnectionListenerBacklog < 1
            || userConnectionListenerBacklog > kMaxUserConnectionListenerBacklog) {
        throw InvalidConfigurationOptionError(
                "User connection listener backlog value is out of range");
    }
    tmpOptions.m_generalOptions.m_userConnectionListenerBacklog = userConnectionListenerBacklog;

    // Parse max number of user connections
    const auto maxUserConnections = config.get<unsigned>(
            constructOptionPath(kGeneralOptionMaxUserConnections), kDefaultMaxUserConnections);
    if (maxUserConnections < 1 || maxUserConnections > kMaxMaxUserConnections) {
        throw InvalidConfigurationOptionError("Max. number of user connections is out of range");
    }
    tmpOptions.m_generalOptions.m_maxUserConnections = maxUserConnections;

    // Log options

    {
        // Collect and validate log channel names
        std::vector<std::string> channels;
        {
            std::unordered_set<std::string> knownChannels;
            const auto value = boost::trim_copy(
                    config.get<std::string>(constructOptionPath(kGeneralOptionLogChannels), ""));
            boost::split(channels, value, boost::is_any_of(","));
            for (auto& v : channels) {
                boost::trim(v);
                if (v.empty())
                    throw InvalidConfigurationOptionError("Empty log channel name detected");
                if (!knownChannels.insert(v).second) {
                    std::ostringstream err;
                    err << "Duplicate log channel name " << v;
                    throw InvalidConfigurationOptionError(err.str());
                }
            }
        }

        // Check that we have at least one log channel
        if (channels.empty()) throw InvalidConfigurationOptionError("No log channels defined");

        // Parse log channel options
        for (const auto& logChannelName : channels) {
            const auto channelOptionPrefix = "log." + logChannelName + ".";

            LogChannelOptions channelOptions;
            channelOptions.m_name = logChannelName;

            // Channel type
            {
                const auto path = constructOptionPath(channelOptionPrefix + kLogChannelOptionType);
                const auto channelType = config.get<std::string>(path, "");
                if (channelType.empty()) {
                    std::ostringstream err;
                    err << "Type not defined for the log channel " << logChannelName;
                    throw InvalidConfigurationOptionError(err.str());
                }
                if (channelType == "console") {
                    channelOptions.m_type = LogChannelType::kConsole;
                } else if (channelType == "file") {
                    channelOptions.m_type = LogChannelType::kFile;
                } else {
                    std::ostringstream err;
                    err << "Unsupported channel type '" << channelType
                        << "' specified for the log channel " << logChannelName;
                    throw InvalidConfigurationOptionError(err.str());
                }
            }

            // Destination
            {
                const auto path =
                        constructOptionPath(channelOptionPrefix + kLogChannelOptionDestination);
                channelOptions.m_destination = boost::trim_copy(config.get<std::string>(path, ""));
                if (channelOptions.m_destination.empty()) {
                    std::ostringstream err;
                    err << "Destination not defined for the log channel " << logChannelName;
                    throw InvalidConfigurationOptionError(err.str());
                }
            }

            // Max. file size
            try {
                const auto path =
                        constructOptionPath(channelOptionPrefix + kLogChannelOptionMaxFileSize);
                auto option = boost::trim_copy(config.get<std::string>(
                        path, std::to_string(defaults::kDefaultMaxLogFileSize / kBytesInMB)));
                off_t multiplier = 0;
                if (option.size() > 1) {
                    const auto lastChar = option.back();
                    switch (lastChar) {
                        case 'k':
                        case 'K': {
                            multiplier = kBytesInKB;
                            break;
                        }
                        case 'm':
                        case 'M': {
                            multiplier = kBytesInMB;
                            break;
                        }
                        case 'g':
                        case 'G': {
                            multiplier = kBytesInGB;
                            break;
                        }
                        default: break;
                    }
                    if (multiplier > 0) {
                        option.erase(option.length() - 1, 1);
                    }
                }
                if (multiplier == 0) {
                    multiplier = kBytesInMB;
                }
                auto value = std::stoll(option);
                if (value == 0) throw std::out_of_range("value is zero");
                const auto upperLimit = defaults::kMaxMaxLogFileSize / multiplier;
                if (value > upperLimit) throw std::out_of_range("value is too big");
                channelOptions.m_maxLogFileSize = value * multiplier;
            } catch (std::exception& ex) {
                std::ostringstream err;
                err << "Invalid value of max. file size for the log channel " << logChannelName
                    << ": " << ex.what();
                throw InvalidConfigurationOptionError(err.str());
            }

            // Max.number of files
            try {
                const auto path =
                        constructOptionPath(channelOptionPrefix + kLogChannelOptionMaxFiles);
                auto option = boost::trim_copy(config.get<std::string>(
                        path, std::to_string(defaults::kDefaultMaxLogFilesCount)));
                auto maxFiles = std::stoull(option);
                if (maxFiles == 0) throw std::out_of_range("value is zero");
                channelOptions.m_maxFiles = maxFiles;
            } catch (std::exception& ex) {
                std::ostringstream err;
                err << "Invalid value of max. number of log files for the log channel "
                    << logChannelName << ": " << ex.what();
                throw InvalidConfigurationOptionError(err.str());
            }

            // Expiration time
            try {
                const auto path =
                        constructOptionPath(channelOptionPrefix + kLogChannelOptionExpirationTime);
                auto option = boost::trim_copy(config.get<std::string>(
                        path, std::to_string(
                                      defaults::kDefaultLogFileExpirationTimeout / kSecondsInDay)));
                std::time_t multiplier = 0;
                if (option.size() > 1) {
                    const auto lastChar = option.back();
                    switch (lastChar) {
                        case 's':
                        case 'S': {
                            multiplier = 1;
                            break;
                        }
                        case 'm':
                        case 'M': {
                            multiplier = kSecondsInMinute;
                            break;
                        }
                        case 'h':
                        case 'H': {
                            multiplier = kSecondsInHour;
                            break;
                        }
                        case 'd':
                        case 'D': {
                            multiplier = kSecondsInDay;
                            break;
                        }
                        case 'w':
                        case 'W': {
                            multiplier = kSecondsInWeek;
                            break;
                        }
                        default: break;
                    }
                    if (multiplier > 1) option.erase(option.length() - 1, 1);
                }
                if (multiplier == 0) multiplier = kSecondsInDay;
                std::time_t value = std::stoul(option);
                const auto upperLimit = defaults::kMaxLogFileExpirationTimeout / multiplier;
                if (value > upperLimit) throw std::out_of_range("value is too big");
                channelOptions.m_logFileExpirationTimeout = value * multiplier;
            } catch (std::exception& ex) {
                std::ostringstream err;
                err << "Invalid value of expiration time for the log channel " << logChannelName
                    << ": " << ex.what();
                throw InvalidConfigurationOptionError(err.str());
            }

            // Severity
            {
                const auto path =
                        constructOptionPath(channelOptionPrefix + kLogChannelOptionSeverity);
                const auto option = boost::trim_copy(config.get<std::string>(
                        path, logLevelNames[static_cast<std::size_t>(
                                      defaults::kDefaultLogSeverityLevel)]));
                std::size_t i = 0;
                for (; i < logLevelNames.size(); ++i) {
                    if (strcasecmp(option.c_str(), logLevelNames[i]) == 0) {
                        channelOptions.m_severity =
                                static_cast<boost::log::trivial::severity_level>(i);
                        break;
                    }
                }
                if (i == logLevelNames.size()) {
                    std::ostringstream err;
                    err << "Invalid log severity level for the log channel " << logChannelName;
                    throw InvalidConfigurationOptionError(err.str());
                }
                channelOptions.m_severity = static_cast<boost::log::trivial::severity_level>(i);
            }

            tmpOptions.m_logOptions.m_logChannels.push_back(std::move(channelOptions));

        }  // for (channels)
    }

    // IOManager options

    // Parse worker thread number
    {
        tmpOptions.m_ioManagerOptions.m_workerThreadNumber =
                config.get<unsigned>(constructOptionPath(kIOManagerOptionWorkerThreadNumber),
                        kDefaultIOManagerWorkerThreadNumber);
        if (tmpOptions.m_ioManagerOptions.m_workerThreadNumber < 1) {
            throw InvalidConfigurationOptionError(
                    "Number of IO Manager worker threads is out of range");
        }
    }

    // Parse writer thread number
    {
        tmpOptions.m_ioManagerOptions.m_writerThreadNumber =
                config.get<unsigned>(constructOptionPath(kIOManagerOptionWriterThreadNumber),
                        kDefaultIOManagerWriterThreadNumber);
        if (tmpOptions.m_ioManagerOptions.m_writerThreadNumber < 1) {
            throw InvalidConfigurationOptionError(
                    "Number of IO Manager writer threads is out of range");
        }
    }

    // Parse IPv4 port number
    {
        tmpOptions.m_ioManagerOptions.m_ipv4port = config.get<int>(
                constructOptionPath(kIOManagerOptionIpv4Port), kDefaultIOManagerIpv4PortNumber);
        if (tmpOptions.m_ioManagerOptions.m_ipv4port != 0
                && (tmpOptions.m_ioManagerOptions.m_ipv4port < kMinPortNumber
                        || tmpOptions.m_ioManagerOptions.m_ipv4port > kMaxPortNumber))
            throw InvalidConfigurationOptionError("Invalid IO Manager IPv4 port number");

        if (tmpOptions.m_ioManagerOptions.m_ipv4port != 0
                && tmpOptions.m_ioManagerOptions.m_ipv4port
                           == tmpOptions.m_generalOptions.m_ipv4port) {
            throw InvalidConfigurationOptionError(
                    "IO Manager and database use the same IPv4 port number");
        }
    }

    // Parse IPv6 port number
    {
        tmpOptions.m_ioManagerOptions.m_ipv6port = config.get<int>(
                constructOptionPath(kIOManagerOptionIpv6Port), kDefaultIOManagerIpv6PortNumber);
        if (tmpOptions.m_ioManagerOptions.m_ipv6port != 0
                && (tmpOptions.m_ioManagerOptions.m_ipv6port < kMinPortNumber
                        || tmpOptions.m_ioManagerOptions.m_ipv6port > kMaxPortNumber))
            throw InvalidConfigurationOptionError("Invalid IO Manager IPv6 port number");

        if (tmpOptions.m_ioManagerOptions.m_ipv6port != 0
                && tmpOptions.m_ioManagerOptions.m_ipv6port
                           == tmpOptions.m_generalOptions.m_ipv6port) {
            throw InvalidConfigurationOptionError(
                    "IO Manager and database use the same IPv6 port number");
        }
    }

    if (tmpOptions.m_ioManagerOptions.m_ipv6port == 0
            && tmpOptions.m_ioManagerOptions.m_ipv4port == 0)
        throw InvalidConfigurationOptionError("Both IPv4 and IPv6 are disabled for IO Manager");

    // Parse block cache capacity
    {
        tmpOptions.m_ioManagerOptions.m_blockCacheCapacity =
                config.get<unsigned>(constructOptionPath(kIOManagerOptionBlockCacheCapacity),
                        kDefaultIOManagerBlockCacheCapacity);
        if (tmpOptions.m_ioManagerOptions.m_blockCacheCapacity < kMinIOManagerBlockCacheCapacity) {
            throw InvalidConfigurationOptionError("IO Manager block cache capacity is too small");
        }
    }

    // Parse user cache capacity
    {
        tmpOptions.m_ioManagerOptions.m_userCacheCapacity =
                config.get<unsigned>(constructOptionPath(kIOManagerOptionUserCacheCapacity),
                        kDefaultIOManagerUserCacheCapacity);
        if (tmpOptions.m_ioManagerOptions.m_userCacheCapacity < kMinIOManagerUserCacheCapacity) {
            throw InvalidConfigurationOptionError("IO Manager user cache capacity is too small");
        }
    }

    // Parse database cache capacity
    {
        tmpOptions.m_ioManagerOptions.m_databaseCacheCapacity =
                config.get<unsigned>(constructOptionPath(kIOManagerOptionDatabaseCacheCapacity),
                        kDefaultIOManagerDatabaseCacheCapacity);
        if (tmpOptions.m_ioManagerOptions.m_databaseCacheCapacity
                < kMinIOManagerDatabaseCacheCapacity) {
            throw InvalidConfigurationOptionError(
                    "IO Manager database cache capacity is too small");
        }
    }

    // Parse table cache capacity
    {
        tmpOptions.m_ioManagerOptions.m_tableCacheCapacity =
                config.get<unsigned>(constructOptionPath(kIOManagerOptionTableCacheCapacity),
                        kDefaultIOManagerTableCacheCapacity);
        if (tmpOptions.m_ioManagerOptions.m_tableCacheCapacity < kMinIOManagerTableCacheCapacity) {
            throw InvalidConfigurationOptionError("IO Manager table cache capacity is too small");
        }
    }

    // Encryption options

    // Parse default cipher ID
    tmpOptions.m_encryptionOptions.m_defaultCipherId = boost::trim_copy(config.get<std::string>(
            constructOptionPath(kEncryptionOptionDefaultCipherId), kDefaultCipherId));

    // Parse system database cipher ID
    tmpOptions.m_encryptionOptions.m_systemDbCipherId = boost::trim_copy(
            config.get<std::string>(constructOptionPath(kEncryptionOptionSystemDbCipherId),
                    tmpOptions.m_encryptionOptions.m_defaultCipherId));

    // Client options

    {
        BoolTranslator translator;
        tmpOptions.m_clientOptions.m_enableEncryption =
                config.get<bool>(constructOptionPath(kClientOptionEnableEncryption),
                        kDefaultClientEnableEncryption, translator);
    }

    if (tmpOptions.m_clientOptions.m_enableEncryption) {
        tmpOptions.m_clientOptions.m_tlsCertificate = boost::trim_copy(
                config.get<std::string>(constructOptionPath(kClientOptionTlsCertificate), ""));

        tmpOptions.m_clientOptions.m_tlsCertificateChain = boost::trim_copy(
                config.get<std::string>(constructOptionPath(kClientOptionTlsCertificateChain), ""));

        tmpOptions.m_clientOptions.m_tlsPrivateKey = boost::trim_copy(
                config.get<std::string>(constructOptionPath(kClientOptionTlsPrivateKey), ""));

        if (tmpOptions.m_clientOptions.m_tlsCertificate.empty()
                && tmpOptions.m_clientOptions.m_tlsCertificate.empty())
            throw std::runtime_error(
                    "Client certificate or certificate chain must be set to create a "
                    "TLS connection");

        if (tmpOptions.m_clientOptions.m_tlsPrivateKey.empty())
            throw std::runtime_error("Client TLS private keys is empty");
    }

    // All options valid, save them
    *this = std::move(tmpOptions);
}

namespace {

boost::property_tree::ptree readConfiguration(const std::string& instanceName)
{
    validateInstance(instanceName);
    const auto instanceConfigFile = composeInstanceConfigFilePath(instanceName);
    boost::property_tree::ptree config;
    boost::property_tree::read_ini(instanceConfigFile, config);
    return config;
}

}  // anonymous namespace

}  // namespace siodb::config
