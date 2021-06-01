# eth-swarm-agent

Swarm Bee active reporting tool.
solve data security and IP management problems without public network.

## feature

1. support auto cash-out
2. support multiple platforms(OSX, Windows, CentOS, Ubuntu, etc...)
3. support for custom data gateway

## how do i use it?

```bash
  ./bee_agent {OPTIONS}

    swarm bee data agent!
    source code: https://github.com/icyblazek/eth-swarm-agent

  OPTIONS:

      -h, --help                        display this help menu
      -n, --nid                         eth-swarm platform node id
      --host                            default localhost
      -d                                default 1635
      -g                                default gateway: api.eth-swarm.io
      --gPort                           default gateway port: 80
      --auto                            auto cashout, default disable
      -t[min]                           upload interval, default 5 min
      --upload                          auto upload data, default enable

    please visit http://eth-swarm.io
```

## thanks
1. [cpp-httplib] https://github.com/yhirose/cpp-httplib
2. [spdlog] https://github.com/gabime/spdlog
3. [nlohmann-json] https://github.com/nlohmann/json
4. [args]https://github.com/Taywee/args
5. [libuv]https://github.com/libuv/libuv 
