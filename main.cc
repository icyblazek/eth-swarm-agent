//
// Created by Icyblazek on 2021/5/27.
// Github https://github.com/icyblazek
//
#include <cstdio>
#include <uv.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include "args.hxx"

#ifdef _WIN32
#else
#include <spdlog/spdlog.h>
#endif // _WIN32

using namespace std;

string g_host = "localhost";
int g_debug_port = 1635;
string g_gateway = "eth-swarm.io";
int g_gateway_port = 80;
bool g_auto_cashout = false;
int g_upload_interval = 300;
httplib::Client *g_httpClient = nullptr;

std::tuple<int, string> bee_health(){
    int res_code = -1;
    string version = "unknown";
    if (auto res = g_httpClient->Get("/health")){
        if (res && res->status == 200){
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("status") && json.at("status").get<string>() == "ok"){
                res_code = 0;
                version = json.at("version").get<string>();
            }
        }
    }else {
        res_code = static_cast<int>(res.error());
    }
    return std::tuple<int, string>(res_code, version);
}

int bee_peers(){
    int peerCount = 0;
    if (auto res = g_httpClient->Get("/peers")){
        if (res && res->status == 200){
            auto json = nlohmann::json::parse(res->body);
            peerCount = json.at("peers").size();
        }
    }
    return peerCount;
}

string bee_address(){
    string address;
    if (auto res = g_httpClient->Get("/addresses")){
        if (res && res->status == 200){
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("ethereum")){
                address = json.at("ethereum").get<string>();
            }
        }
    }
    return address;
}

string bee_chequebook_address() {
    string address;
    if (auto res = g_httpClient->Get("/chequebook/address")){
        if (res && res->status == 200){
            /**
             * after bee 0.6.0,  chequebookaddress change to chequebookAddress
             */
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("chequebookaddress")){
                address = json.at("chequebookaddress").get<string>();
            }else if (json.contains("chequebookAddress")){
                address = json.at("chequebookAddress").get<string>();
            }
        }
    }
    return address;
}

std::tuple<int, std::list<nlohmann::json>> bee_lastcheques() {
    int res_code = -1;
    string body;
    std::list<nlohmann::json> peers;
    if (auto res = g_httpClient->Get("/chequebook/cheque")){
        if (res && res->status == 200){
            res_code = 0;
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("lastcheques")){
                nlohmann::json::array_t lastcheques = json.at("lastcheques");
                for (auto & lastcheque : lastcheques){
                    peers.push_back(lastcheque);
                }
            }
        }
    }else {
        res_code = static_cast<int>(res.error());
    }
    return std::tuple<int, std::list<nlohmann::json>>(res_code, peers);
}

long bee_get_cumulative_payout(const string& peer){
    long payout = 0;
    
    if (auto res = g_httpClient->Get(("/chequebook/cheque/" + peer).c_str())){
        if (res && res->status == 200){
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("lastreceived")){
                auto lastreceived = json.at("lastreceived");
                if (lastreceived.contains("payout")){
                    payout = lastreceived.at("payout").get<long>();
                }
            }
        }
    }
    return payout;
}

long bee_get_last_cashed_payout(const string& peer){
    long payout = 0;

    if (auto res = g_httpClient->Get(("/chequebook/cashout/" + peer).c_str())){
        if (res && res->status == 200){
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("cumulativePayout")){
                payout = json.at("cumulativePayout").get<long>();
            }
        }
    }
    return payout;
}

long bee_get_uncashed_amount(const string& peer){
    auto cumulativePayout = bee_get_cumulative_payout(peer);
    if (cumulativePayout <= 0){
        return 0;
    }
    auto cashedPayout = bee_get_last_cashed_payout(peer);
    return cumulativePayout - cashedPayout;
}

bool bee_cashout(const string& peer, long uncashedAmount){
    bool success = false;
    spdlog::info("uncashed cheque for {} ({} uncashed)", peer, uncashedAmount);
    if (auto res = g_httpClient->Post(("/chequebook/cashout/" + peer).c_str())){
        if (res && res->status == 200){
            auto json = nlohmann::json::parse(res->body);
            if (json.contains("transactionHash")){
                auto transactionHash = json.at("transactionHash").get<string>();
                if (transactionHash.length() > 0){
                    success = true;
                    spdlog::info("cashing out cheque for {} in transaction {}", peer, transactionHash);
                }
            }else {
                auto code = json.at("code").get<int>();
                auto message = json.at("message").get<string>();
                spdlog::error("cashout fail, code: {}, message: {}", code, message);
            }
        }
    }
    return success;
}

void timer_cb(uv_timer_t* handle){
    if (g_httpClient == nullptr){
        uv_timer_stop(reinterpret_cast<uv_timer_t *>(&handle));
    }

    nlohmann::json data;
    auto health = bee_health();
    int result_code = std::get<0>(health);
    if (result_code == 0){
        data["status"] = "ok";
        data["address"] = bee_address();
        data["chequebookAddress"] = bee_chequebook_address();
        data["peersCount"] = bee_peers();

        auto lastcheques = bee_lastcheques();
        long totalAmount = 0;
        long uncashedAmount = 0;
        int availableTicket = 0;
        if (std::get<0>(lastcheques) == 0){
            std::list<nlohmann::json> cheques = std::get<1>(lastcheques);
            for (auto & cheque : cheques){
                if (cheque.contains("lastreceived")){
                    auto lastReceived = cheque.at("lastreceived");
                    if (lastReceived.contains("payout")){
                        availableTicket++;
                        totalAmount += lastReceived.at("payout").get<long>();
                    }
                }
                auto peer = cheque.at("peer").get<string>();
                auto t_uncashedAmount = bee_get_uncashed_amount(peer);
                if (t_uncashedAmount > 0 && g_auto_cashout){
                    bee_cashout(peer, uncashedAmount);
                }
                uncashedAmount += t_uncashedAmount;
            }
        }
        data["totalAmount"] = totalAmount;
        data["uncashedAmount"] = uncashedAmount;
    }else {
        data["status"] = "fail";
    }

    httplib::Client client(g_gateway, 80);
    auto postData = data.dump();
    auto res = client.Post("/agent/upload", postData, "application/json");
    if (res && res->status == 200){
        spdlog::info("data upload success! {}", postData);
    }else {
        spdlog::info("data upload fail! {}", postData);
    }
}

int main(int argc, char **argv) {
    args::ArgumentParser parser("swarm bee data agent!\nsource code: https://github.com/icyblazek/eth-swarm-agent", "please visit http://eth-swarm.io");
    args::HelpFlag help(parser, "help", "display this help menu", {'h', "help"});
    args::ValueFlag<string> node_id(parser, "", "eth-swarm platform node id", {'n', "nid"});
    args::ValueFlag<string> host(parser, "", "default localhost", {"host"}, "localhost");
    args::ValueFlag<int> debug_port(parser, "", "default 1635", {'d'}, 1635);
    args::ValueFlag<string> gateway(parser, "", "default gateway: eth-swarm.io", {'g'}, "eth-swarm.io");
    args::ValueFlag<int> gateway_port(parser, "", "default gateway port: 80", {"gPort"}, 80);
    args::ValueFlag<bool> auto_cashout(parser, "", "auto cashout, default disable", {"auto"}, false);
    args::ValueFlag<int> upload_interval(parser, "seconds", "upload interval, default 5 min", {'t'}, 300);
    try {
        parser.ParseCLI(argc, argv);
    }catch(const args::Help&){
        std::cout << parser;
        return 0;
    }catch(const args::ParseError &e){
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return 1;
    }

    if (!node_id){
        spdlog::error("node_id can't be empty!!!");
        std::cerr << parser;
        return 1;
    }

    printf("@@@@@@@@-xxx-=@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    printf("@@@@@-xx      x-=@@@@@@@@@@===================-@@@@@=------------====-=@@@@@=================-@@@@@=----=@@@@@-----@@@@@\n");
    printf("@=-x    xxxxx    x-=@@@@@@@                   x@@@@@xxxxxxxxxxxxx     -@@@@= xxxxxxxxxxxxxxxx =@@@=        xxx       xxx\n");
    printf("x    xx--xxx--x     x@@@@@@=====@=x    -@@@@@@@@@@@@-xxxxxxxxxxx-x    =@@@@@@@@@@@@@@@@@@@@@@@@@@@xxxx-xxxx-xxxxx-xxx-@@\n");
    printf("    x-xx  xx=xx-x    @@@@@@@@@@@@=    x@@@@@@@@@@@@@-xxxxxxxxxxxx    x@@@@@@@@@@@@@@@@@@@@@@@@@@@@=xxxxxxxxxxxxxxxxxxx@@\n");
    printf("    x-x---x-=  x=x  x@@@@========-    x========@@@@xxxxxxxxxxxxxx    x@@@    xxxxxx           x@@@-    xxxxxxxxxx    -@@\n");
    printf("    --xx--x--  x-   x@@@@                     x@@@@------------xxxxx-=@@@----x     x-----------@@@x   x---------x    =@@\n");
    printf("    -x   x-=-  --   x@@@@========x    -=======@@@xxxxxxxxxxxxxxxxxxxxxx@@@@@=x    -@@@=----@@@@@@@x   xxxxxxxxxxx   x@@@\n");
    printf("     x--x  x----x    @@@@@@@@@@@=    x@@@@@@@@@@@-------------x    x---@@@@-    x=@@@@=    -@@@@@=xx    xxxxxxx    x-@@@\n");
    printf("x      x----xxx     x@@@@@@@@@@@-    x@@@@@@@@@@@@@@xxxxx@@@@@-    -@@@@@@x    x=@@@@@=x   x@@@@xxxx    xxxxxxx    xxx@@\n");
    printf("@=-x     xx      x-=@@@@@@@@@@@@x    -@@@@@@@@@@@@@@-    x@@@=x    =@@@@@x     xx           =@@@=-    x-=====x    x===@@\n");
    printf("@@@@=-x       x-=@@@@@@@@@@@@@@@-xxxx=@@@@@@@@@@@@@@@=====@@=xxxxx-@@@@@@--------=======xxxx-@@@@-xxxx=@@@@@@-xxxx=@@@@@\n");
    printf("@@@@@@@=-x x-=@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    printf("\n");

    g_host = args::get(host);
    g_debug_port = args::get(debug_port);
    g_gateway = args::get(gateway);
    g_gateway_port = args::get(gateway_port);
    g_auto_cashout = args::get(auto_cashout);
    g_upload_interval = args::get(upload_interval);

    printf("agent started! \nbee host: %s, debug port: %d, node_id: %s, gateway: %s:%d\n", g_host.c_str(), g_debug_port, args::get(node_id).c_str(), g_gateway.c_str(), g_gateway_port);

    g_httpClient = new httplib::Client(g_host, g_debug_port);
    g_httpClient->set_keep_alive(true);

    uv_loop_t *main_loop = uv_default_loop();

    uv_timer_s timer_req{};
    uv_timer_init(main_loop, &timer_req);
    // default 5 min repeat
    uv_timer_start(&timer_req, timer_cb, 5000, g_upload_interval);
    uv_run(main_loop, UV_RUN_DEFAULT);
    uv_stop(main_loop);
    uv_loop_close(main_loop);
    free(main_loop);
    return 0;
}