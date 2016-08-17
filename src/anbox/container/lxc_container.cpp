/*
 * Copyright (C) 2016 Simon Fels <morphis@gravedo.de>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "anbox/container/lxc_container.h"
#include "anbox/utils.h"
#include "anbox/config.h"
#include "anbox/logger.h"

#include <stdexcept>
#include <map>

#include <boost/throw_exception.hpp>
#include <boost/filesystem.hpp>

#include <sys/wait.h>
#include <sys/capability.h>
#include <sys/types.h>
#include <sys/prctl.h>

namespace fs = boost::filesystem;

namespace anbox {
namespace container {
LxcContainer::LxcContainer() :
    state_(State::inactive),
    container_(nullptr) {
    utils::ensure_paths({
        config::container_config_path(),
        config::log_path(),
        config::host_share_path(),
        config::host_android_data_path(),
        config::host_android_cache_path(),
        config::host_android_storage_path(),
    });
}

LxcContainer::~LxcContainer() {
    DEBUG("");

    stop();

    if (container_)
        lxc_container_put(container_);
}

void LxcContainer::start(const Configuration &configuration) {
    if (getuid() != 0)
        BOOST_THROW_EXCEPTION(std::runtime_error("You have to start the container as root"));

    if (container_ && container_->is_running(container_))
        BOOST_THROW_EXCEPTION(std::runtime_error("Container already started"));

    if (!container_) {
        DEBUG("Containers are stored in %s", config::container_config_path());

        // Remove container config to be be able to rewrite it
        ::unlink(utils::string_format("%s/default/config", config::container_config_path()).c_str());

        container_ = lxc_container_new("default", config::container_config_path().c_str());
        if (!container_)
            BOOST_THROW_EXCEPTION(std::runtime_error("Failed to create LXC container instance"));
    }

    // We drop all not needed capabilities
    set_config_item("lxc.cap.drop", "mac_admin mac_override sys_time sys_module sys_rawio");

    // We can mount proc/sys as rw here as we will run the container unprivileged in the end
    set_config_item("lxc.mount.auto", "proc:mixed sys:mixed cgroup:mixed");

    set_config_item("lxc.autodev", "1");
    set_config_item("lxc.pts", "1024");
    set_config_item("lxc.tty", "0");
    set_config_item("lxc.utsname", "anbox");

    set_config_item("lxc.group.devices.deny","");
    set_config_item("lxc.group.devices.allow","");

    // We can't move bind-mounts, so don't use /dev/lxc/
    set_config_item("lxc.devttydir", "");

    set_config_item("lxc.environment", "PATH=/system/bin:/system/sbin:/system/xbin");

    set_config_item("lxc.init_cmd", "/anbox-init.sh");
    set_config_item("lxc.rootfs.backend", "dir");

    DEBUG("Using rootfs path %s", config::rootfs_path());
    set_config_item("lxc.rootfs", config::rootfs_path());

    set_config_item("lxc.loglevel", "0");
    set_config_item("lxc.logfile", utils::string_format("%s/container.log", config::log_path()).c_str());

#if 0
    // Android uses namespaces as well so we have to allow nested namespaces for LXC
    // which are otherwise forbidden by AppArmor.
    set_config_item("lxc.aa_profile", "lxc-container-default-with-nesting");
#else
    // FIXME: when using the nested profile we still get various denials from things
    // Android tries to do but isn't allowed to. We need to look into those and see
    // how we can switch back to a confined way of running the container.
    set_config_item("lxc.aa_profile", "unconfined");
#endif

    for (const auto &bind_mount : configuration.bind_mounts) {
        std::string create_type = "file";

        if (fs::is_directory(bind_mount.first))
            create_type = "dir";

        auto target_path = bind_mount.second;
        // LXC wants target paths relative to the container rootfs so
        // prividing an absolute path doesn't work.
        if (utils::string_starts_with(target_path, "/"))
            target_path.erase(0, 1);

        set_config_item("lxc.mount.entry",
            utils::string_format("%s %s none bind,create=%s,optional 0 0",
                                 bind_mount.first, target_path, create_type));
    }

    if (!container_->save_config(container_, nullptr))
        BOOST_THROW_EXCEPTION(std::runtime_error("Failed to save container configuration"));

    if (not container_->start(container_, 0, nullptr))
        BOOST_THROW_EXCEPTION(std::runtime_error("Failed to start container"));

    state_ = Container::State::running;

    DEBUG("Container successfully started");
}

void LxcContainer::stop() {
    if (not container_ || not container_->is_running(container_))
        BOOST_THROW_EXCEPTION(std::runtime_error("Cannot stop container as it is not running"));

    if (not container_->stop(container_))
        BOOST_THROW_EXCEPTION(std::runtime_error("Failed to stop container"));

    state_ = Container::State::inactive;

    DEBUG("Container successfully stopped");
}

void LxcContainer::set_config_item(const std::string &key, const std::string &value) {
    if (!container_->set_config_item(container_, key.c_str(), value.c_str()))
        BOOST_THROW_EXCEPTION(std::runtime_error("Failed to configure LXC container"));
}

Container::State LxcContainer::state() {
    return state_;
}
} // namespace container
} // namespace anbox