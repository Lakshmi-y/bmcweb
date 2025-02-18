/*
 // Copyright (c) 2018 Intel Corporation
 //
 // Licensed under the Apache License, Version 2.0 (the "License");
 // you may not use this file except in compliance with the License.
 // You may obtain a copy of the License at
 //
 //      http://www.apache.org/licenses/LICENSE-2.0
 //
 // Unless required by applicable law or agreed to in writing, software
 // distributed under the License is distributed on an "AS IS" BASIS,
 // WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 // See the License for the specific language governing permissions and
 // limitations under the License.
 */
#pragma once

#include "dbus_singleton.hpp"

#include <boost/system/error_code.hpp>
#include <sdbusplus/asio/property.hpp>
#include <sdbusplus/message.hpp>
#include <sdbusplus/unpack_properties.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <regex>
#include <string>
#include <tuple>
#include <vector>

namespace dbus
{

namespace utility
{

using DbusVariantType =
    std::variant<std::vector<std::tuple<std::string, std::string, std::string>>,
                 std::vector<std::string>, std::vector<double>, std::string,
                 int64_t, uint64_t, double, int32_t, uint32_t, int16_t,
                 uint16_t, uint8_t, bool>;

using DBusPropertiesMap =
    boost::container::flat_map<std::string, DbusVariantType>;
using DBusInteracesMap =
    boost::container::flat_map<std::string, DBusPropertiesMap>;
using ManagedObjectType =
    std::vector<std::pair<sdbusplus::message::object_path, DBusInteracesMap>>;

using ManagedItem = std::pair<
    sdbusplus::message::object_path,
    boost::container::flat_map<
        std::string, boost::container::flat_map<std::string, DbusVariantType>>>;

// Map of service name to list of interfaces
using MapperServiceMap =
    std::vector<std::pair<std::string, std::vector<std::string>>>;

// Map of object paths to MapperServiceMaps
using MapperGetSubTreeResponse =
    std::vector<std::pair<std::string, MapperServiceMap>>;

using MapperGetSubTreePathsResponse = std::vector<std::string>;

using MapperGetObject =
    std::vector<std::pair<std::string, std::vector<std::string>>>;

using MapperEndPoints = std::vector<std::string>;

inline void escapePathForDbus(std::string& path)
{
    const std::regex reg("[^A-Za-z0-9_/]");
    std::regex_replace(path.begin(), path.begin(), path.end(), reg, "_");
}

// gets the string N strings deep into a path
// i.e.  /0th/1st/2nd/3rd
inline bool getNthStringFromPath(const std::string& path, int index,
                                 std::string& result)
{
    if (index < 0)
    {
        return false;
    }

    std::filesystem::path p1(path);
    int count = -1;
    for (auto const& element : p1)
    {
        if (element.has_filename())
        {
            ++count;
            if (count == index)
            {
                result = element.stem().string();
                break;
            }
        }
    }
    if (count < index)
    {
        return false;
    }

    return true;
}

template <typename Callback>
inline void checkDbusPathExists(const std::string& path, Callback&& callback)
{
    using GetObjectType =
        std::vector<std::pair<std::string, std::vector<std::string>>>;

    crow::connections::systemBus->async_method_call(
        [callback{std::move(callback)}](const boost::system::error_code ec,
                                        const GetObjectType& objectNames) {
            callback(!ec && objectNames.size() != 0);
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetObject", path,
        std::array<std::string, 0>());
}

template <std::size_t N>
inline void
    getSubTree(const std::string& path, int32_t depth,
               const std::array<const char*, N>& interfaces,
               std::function<void(const boost::system::error_code&,
                                  const MapperGetSubTreeResponse&)>&& callback)
{
    crow::connections::systemBus->async_method_call(
        [callback](const boost::system::error_code ec,
                   const MapperGetSubTreeResponse& subtree) {
            callback(ec, subtree);
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree", path, depth,
        interfaces);
}

template <std::size_t N>
inline void getSubTreePaths(
    const std::string& path, int32_t depth,
    const std::array<const char*, N>& interfaces,
    std::function<void(const boost::system::error_code&,
                       const MapperGetSubTreePathsResponse&)>&& callback)
{
    crow::connections::systemBus->async_method_call(
        [callback](const boost::system::error_code ec,
                   const MapperGetSubTreePathsResponse& subtreePaths) {
            callback(ec, subtreePaths);
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTreePaths", path, depth,
        interfaces);
}

inline void getAssociationEndPoints(
    const std::string& path,
    std::function<void(const boost::system::error_code&,
                       const MapperEndPoints&)>&& callback)
{
    sdbusplus::asio::getProperty<MapperEndPoints>(
        *crow::connections::systemBus, "xyz.openbmc_project.ObjectMapper", path,
        "xyz.openbmc_project.Association", "endpoints",
        [callback](const boost::system::error_code& ec,
                   const MapperEndPoints& endpoints) {
            callback(ec, endpoints);
        });
}

// NOTE: getAssociatedSubTreePaths() is implemented without using the DBUS
// primitive function "GetAssociatedSubTreePaths"

template <std::size_t N>
inline void validateAssociatedSubTreePaths(
    const sdbusplus::message::object_path& associatedPath,
    const dbus::utility::MapperEndPoints& endpoints,
    const sdbusplus::message::object_path& path, int32_t depth,
    const std::array<const char*, N>& interfaces,
    std::function<void(const boost::system::error_code&,
                       const MapperGetSubTreePathsResponse&)>&& callback)
{
    // Walk thru SubTree of path and check whether it matches with
    // interfaces
    getSubTreePaths(
        path, depth, interfaces,
        [associatedPath, endpoints, path, depth, interfaces, callback](
            const boost::system::error_code ec,
            const dbus::utility::MapperGetSubTreePathsResponse& subtreePaths) {
            MapperGetSubTreePathsResponse associatedSubtreePaths;
            if (ec || subtreePaths.empty())
            {
                // callback with ec & empty subtreePaths
                callback(ec, associatedSubtreePaths);
                return;
            }

            // Build a set of endpoints for the quicker search
            std::set<std::string> endpointSet(endpoints.begin(),
                                              endpoints.end());

            for (const auto& objectPath : subtreePaths)
            {
                if (endpointSet.find(objectPath) != endpointSet.end())
                {
                    associatedSubtreePaths.emplace_back(objectPath);
                }
            }

            std::sort(associatedSubtreePaths.begin(),
                      associatedSubtreePaths.end());
            callback(ec, associatedSubtreePaths);
        });
}

template <std::size_t N>
inline void getAssociatedSubTreePaths(
    const sdbusplus::message::object_path& associatedPath,
    const sdbusplus::message::object_path& path, int32_t depth,
    const std::array<const char*, N>& interfaces,
    std::function<void(const boost::system::error_code&,
                       const MapperGetSubTreePathsResponse&)>&& callback)
{
    getAssociationEndPoints(
        associatedPath, [associatedPath, path, depth, interfaces, callback](
                            const boost::system::error_code& ec,
                            const dbus::utility::MapperEndPoints& endpoints) {
            if (ec || endpoints.empty())
            {
                // callback with ec & empty subtreePaths
                MapperGetSubTreePathsResponse subtreePaths;
                callback(ec, subtreePaths);
                return;
            }

            validateAssociatedSubTreePaths(
                associatedPath, endpoints, path, depth, interfaces,
                [callback](const boost::system::error_code& ec,
                           const dbus::utility::MapperGetSubTreePathsResponse&
                               subtreePaths) { callback(ec, subtreePaths); });
        });
}

} // namespace utility
} // namespace dbus
