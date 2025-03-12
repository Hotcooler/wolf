#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_container_properties.hpp>
#include <catch2/matchers/catch_matchers_contains.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <core/docker.hpp>
#include <docker/json_formatters.hpp>
#include <rfl/toml.hpp>
#include <runners/docker.hpp>
#include <state/config.hpp>
#include <state/serialised_config.hpp>

using Catch::Matchers::Contains;
using Catch::Matchers::Equals;

TEST_CASE("Docker API", "[DOCKER]") {
  docker::init();
  auto docker_socket = utils::get_env("DOCKER_SOCKET_PATH", "/var/run/docker.sock");
  docker::DockerAPI docker_api(docker_socket);

  docker::Container container = {
      .id = "",
      .name = "WolfTestHelloWorld",
      .image = "hello-world",
      .status = docker::CREATED,
      .ports = {docker::Port{.private_port = 1234, .public_port = 1235, .type = docker::TCP}},
      .mounts = {docker::MountPoint{.source = "/tmp/", .destination = "/tmp/", .mode = "ro"}},
      .devices = {docker::Device{.path_on_host = "/dev/input/mice",
                                 .path_in_container = "/dev/input/mice",
                                 .cgroup_permission = "mrw"}},
      .env = {"ASD=true"}};

  auto first_container = docker_api.create(container);
  REQUIRE(first_container.has_value());
  REQUIRE(docker_api.start_by_id(first_container.value().id));
  REQUIRE(docker_api.stop_by_id(first_container.value().id));

  // This should remove the first container and create a new one with the same name
  auto second_container = docker_api.create(first_container.value(), R"({
    "Env": ["AN_ENV_VAR=true"],
    "HostConfig": {
      "IpcMode": "host"
    }
  })");
  REQUIRE(second_container.has_value());
  REQUIRE(first_container->id != second_container->id);
  REQUIRE(first_container->name == second_container->name);

  REQUIRE_THAT(second_container->env, Contains("AN_ENV_VAR=true"));
  REQUIRE_THAT(second_container->env, Contains("ASD=true"));

  REQUIRE(second_container->ports.size() == first_container->ports.size());
  REQUIRE(second_container->devices.size() == first_container->devices.size());
  REQUIRE(second_container->mounts.size() == first_container->mounts.size());

  REQUIRE(!docker_api.remove_by_id(first_container->id)); // This container doesn't exist anymore
  REQUIRE(docker_api.remove_by_id(second_container->id));
}

TEST_CASE("Docker TOML", "[DOCKER]") {
  docker::init();
  docker::DockerAPI docker_api;

  auto event_bus = std::make_shared<events::EventBusType>();
  std::string toml_cfg = R"(

    type = "docker"
    name = "WolfTestHelloWorld"
    image = "hello-world"
    mounts = [
      "/tmp/sockets:/tmp/.X11-unix/",
      "/tmp/sockets:/run/user/1000/pulse/:ro"
    ]
    devices = [
      "/dev/input/mice:/dev/input/mice:ro",
      "/a/b/c:/d/e/f",
      "/tmp:/tmp:rw",
    ]
    ports = [
      "1234:1235",
      "1234:1235:udp"
    ]
    env = [
      "LOG_LEVEL=info"
    ]
    base_create_json = "{'HostConfig': {}}"

    )";
  std::istringstream is(toml_cfg, std::ios_base::binary | std::ios_base::in);
  // Round trip: load TOML -> serialize back
  auto runner = state::get_runner(rfl::toml::read<wolf::config::AppDocker>(is).value(), event_bus);
  auto container = rfl::get<wolf::config::AppDocker>(runner->serialize().variant());

  REQUIRE_THAT(container.name, Equals("WolfTestHelloWorld"));
  REQUIRE_THAT(container.image, Equals("hello-world"));

  REQUIRE_THAT(container.ports, Equals(std::vector<std::string>{"1234:1235/tcp", "1234:1235/udp"}));
  REQUIRE_THAT(container.devices,
               Equals(std::vector<std::string>{
                   "/dev/input/mice:/dev/input/mice:ro",
                   "/a/b/c:/d/e/f:mrw",
                   "/tmp:/tmp:rw",
               }));
  REQUIRE_THAT(container.env, Equals(std::vector<std::string>{"LOG_LEVEL=info"}));
  REQUIRE_THAT(container.base_create_json.value(), Equals("{'HostConfig': {}}"));
}

TEST_CASE("Parse nulls in json reply", "[DOCKER]") {
  auto reply = R""""(
{
  "Id": "f2eb121b3cf4dfa4e96502675c41b26d660934f324c5c57af9d9baa6730c81cc",
  "Created": "2024-10-23T03:15:27.327380595Z",
  "Path": "tailscaled",
  "Args": [],
  "State": {
    "Status": "running",
    "Running": true,
    "Paused": false,
    "Restarting": false,
    "OOMKilled": false,
    "Dead": false,
    "Pid": 2656,
    "ExitCode": 0,
    "Error": "",
    "StartedAt": "2024-10-23T12:20:44.086062876Z",
    "FinishedAt": "2024-10-23T12:20:07.067144588Z"
  },
  "Image": "sha256:8841c6e652e3b9d1bc299b80d6cfce8dfc0f183305fa88605557812fcf0e1b4d",
  "ResolvConfPath": "/var/lib/docker/containers/f2eb121b3cf4dfa4e96502675c41b26d660934f324c5c57af9d9baa6730c81cc/resolv.conf",
  "HostnamePath": "/var/lib/docker/containers/f2eb121b3cf4dfa4e96502675c41b26d660934f324c5c57af9d9baa6730c81cc/hostname",
  "HostsPath": "/var/lib/docker/containers/f2eb121b3cf4dfa4e96502675c41b26d660934f324c5c57af9d9baa6730c81cc/hosts",
  "LogPath": "/var/lib/docker/containers/f2eb121b3cf4dfa4e96502675c41b26d660934f324c5c57af9d9baa6730c81cc/f2eb121b3cf4dfa4e96502675c41b26d660934f324c5c57af9d9baa6730c81cc-json.log",
  "Name": "/tailscale-docker",
  "RestartCount": 0,
  "Driver": "overlay2",
  "Platform": "linux",
  "MountLabel": "",
  "ProcessLabel": "",
  "AppArmorProfile": "docker-default",
  "ExecIDs": null,
  "HostConfig": {
    "Binds": null,
    "ContainerIDFile": "",
    "LogConfig": {
      "Type": "json-file",
      "Config": {}
    },
    "NetworkMode": "vpn-server_networks",
    "PortBindings": {},
    "RestartPolicy": {
      "Name": "unless-stopped",
      "MaximumRetryCount": 0
    },
    "AutoRemove": false,
    "VolumeDriver": "",
    "VolumesFrom": null,
    "ConsoleSize": [
      0,
      0
    ],
    "CapAdd": [
      "net_admin",
      "sys_module"
    ],
    "CapDrop": null,
    "CgroupnsMode": "private",
    "Dns": [],
    "DnsOptions": [],
    "DnsSearch": [],
    "ExtraHosts": [],
    "GroupAdd": null,
    "IpcMode": "private",
    "Cgroup": "",
    "Links": null,
    "OomScoreAdj": 0,
    "PidMode": "",
    "Privileged": false,
    "PublishAllPorts": false,
    "ReadonlyRootfs": false,
    "SecurityOpt": null,
    "UTSMode": "",
    "UsernsMode": "",
    "ShmSize": 67108864,
    "Runtime": "runc",
    "Isolation": "",
    "CpuShares": 0,
    "Memory": 0,
    "NanoCpus": 0,
    "CgroupParent": "",
    "BlkioWeight": 0,
    "BlkioWeightDevice": null,
    "BlkioDeviceReadBps": null,
    "BlkioDeviceWriteBps": null,
    "BlkioDeviceReadIOps": null,
    "BlkioDeviceWriteIOps": null,
    "CpuPeriod": 0,
    "CpuQuota": 0,
    "CpuRealtimePeriod": 0,
    "CpuRealtimeRuntime": 0,
    "CpusetCpus": "",
    "CpusetMems": "",
    "Devices": null,
    "DeviceCgroupRules": null,
    "DeviceRequests": null,
    "MemoryReservation": 0,
    "MemorySwap": 0,
    "MemorySwappiness": null,
    "OomKillDisable": null,
    "PidsLimit": null,
    "Ulimits": null,
    "CpuCount": 0,
    "CpuPercent": 0,
    "IOMaximumIOps": 0,
    "IOMaximumBandwidth": 0
  },
  "Mounts": [
    {
      "Type": "bind",
      "Source": "/dev/net/tun",
      "Destination": "/dev/net/tun",
      "Mode": "rw",
      "RW": true,
      "Propagation": "rprivate"
    },
    {
      "Type": "bind",
      "Source": "/srv/mergerfs/mergerfs_data/user-data/docker-appdata/tailscale/tailscale_var_lib",
      "Destination": "/var/lib",
      "Mode": "rw",
      "RW": true,
      "Propagation": "rprivate"
    }
  ],
  "Config": {
    "Hostname": "OMV-tailscaleVPN",
    "Domainname": "",
    "User": "",
    "AttachStdin": false,
    "AttachStdout": true,
    "AttachStderr": true,
    "Tty": false,
    "OpenStdin": false,
    "StdinOnce": false,
    "Env": null,
    "Cmd": [
      "tailscaled"
    ],
    "Image": "tailscale/tailscale:stable",
    "Volumes": null,
    "WorkingDir": "",
    "Entrypoint": null,
    "OnBuild": null
  }
}
)"""";

  auto json = utils::parse_json(reply);
  auto parsed_container = boost::json::value_to<wolf::core::docker::Container>(json);

  REQUIRE_THAT(parsed_container.id, Equals("f2eb121b3cf4dfa4e96502675c41b26d660934f324c5c57af9d9baa6730c81cc"));
  REQUIRE(parsed_container.mounts.empty());
  REQUIRE(parsed_container.devices.empty());
  REQUIRE(parsed_container.env.empty());
  REQUIRE(parsed_container.ports.empty());
}