/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/transport/transport_layer_manager.h"

#include "mongo/base/status.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/service_executor_adaptive.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_asio.h"
#include "mongo/transport/transport_layer_legacy.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/time_support.h"
#include <limits>

#include <iostream>

namespace mongo {
namespace transport {

TransportLayerManager::TransportLayerManager() = default;

Ticket TransportLayerManager::sourceMessage(const SessionHandle& session,
                                            Message* message,
                                            Date_t expiration) {
	//TransportLayerASIO::sourceMessage
	return session->getTransportLayer()->sourceMessage(session, message, expiration);
}

Ticket TransportLayerManager::sinkMessage(const SessionHandle& session,
                                          const Message& message,
                                          Date_t expiration) {
    //TransportLayerASIO::sinkMessage
    return session->getTransportLayer()->sinkMessage(session, message, expiration);
}

Status TransportLayerManager::wait(Ticket&& ticket) {
    return getTicketTransportLayer(ticket)->wait(std::move(ticket));
}

void TransportLayerManager::asyncWait(Ticket&& ticket, TicketCallback callback) {
    return getTicketTransportLayer(ticket)->asyncWait(std::move(ticket), std::move(callback));
}

template <typename Callable>
	//TransportLayerManager::shutdown调用，遍历tls，执行cb
void TransportLayerManager::_foreach(Callable&& cb) const {
    {
        stdx::lock_guard<stdx::mutex> lk(_tlsMutex);
        for (auto&& tl : _tls) {
            cb(tl.get());
        }
    }
}

void TransportLayerManager::end(const SessionHandle& session) {
    session->getTransportLayer()->end(session);
}

// TODO Right now this and setup() leave TLs started if there's an error. In practice the server
// exits with an error and this isn't an issue, but we should make this more robust.
//TransportLayerASIO::start  accept处理
//TransportLayerASIO::setup() listen监听
//实际上accept处理走_initAndListen->TransportLayerASIO::start流程，该接口已经没有使用
Status TransportLayerManager::start() {
    for (auto&& tl : _tls) {
		//TransportLayerASIO::start，开始accept初始化处理
        auto status = tl->start();  
        if (!status.isOK()) {
            _tls.clear();
            return status;
        }
    }

    return Status::OK();
}

void TransportLayerManager::shutdown() {
	//TransportLayerASIO::shutdown，传输层回收处理
    _foreach([](TransportLayer* tl) { tl->shutdown(); });
}

//TransportLayerASIO::start  accept处理
//TransportLayerASIO::setup() listen监听
// TODO Same comment as start() 

//实际上accept处理走_initAndListen->TransportLayerASIO::start流程，该接口已经没有使用
//runMongosServer _initAndListen中运行
Status TransportLayerManager::setup() {
    //_tls来源见TransportLayerManager::createWithConfig返回的retVector
    for (auto&& tl : _tls) {
		//TransportLayerASIO::setup() listen监听
        auto status = tl->setup(); 
        if (!status.isOK()) {
            _tls.clear();
            return status;
        }
    }

    return Status::OK();
}

//实际上该接口没用，用得是TransportLayerManager::start
Status TransportLayerManager::addAndStartTransportLayer(std::unique_ptr<TransportLayer> tl) {
    auto ptr = tl.get();
    {
        stdx::lock_guard<stdx::mutex> lk(_tlsMutex);
        _tls.emplace_back(std::move(tl));
    }
	//TransportLayerASIO::start
    return ptr->start();
}

//根据配置构造相应类信息  _initAndListen中调用
std::unique_ptr<TransportLayer> TransportLayerManager::createWithConfig(
    const ServerGlobalParams* config, ServiceContext* ctx) {
    std::unique_ptr<TransportLayer> transportLayer;
	//服务类型，也就是本实例是mongos还是mongod
	//mongos对应ServiceEntryPointMongod,mongod对应ServiceEntryPointMongos
    auto sep = ctx->getServiceEntryPoint();
	//net.transportLayer配置模式，默认asio, legacy模式已淘汰
    if (config->transportLayer == "asio") {
		//获取asio模式对应子配置信息
        transport::TransportLayerASIO::Options opts(config);

		//同步方式还是异步方式，默认synchronous
        if (config->serviceExecutor == "adaptive") {
			//动态线程池模型,也就是异步模式
            opts.transportMode = transport::Mode::kAsynchronous;
        } else if (config->serviceExecutor == "synchronous") {
            //一个链接一个线程模型，也就是同步模式
            opts.transportMode = transport::Mode::kSynchronous;
        } else {
            MONGO_UNREACHABLE;
        }

		//如果配置是asio,构造TransportLayerASIO类
        auto transportLayerASIO = stdx::make_unique<transport::TransportLayerASIO>(opts, sep);

		//ServiceExecutorSynchronous对应线程池同步模式，ServiceExecutorAdaptive对应线程池异步自适应模式
		if (config->serviceExecutor == "adaptive") { //异步方式
			//构造动态线程模型对应的执行器ServiceExecutorAdaptive
            ctx->setServiceExecutor(stdx::make_unique<ServiceExecutorAdaptive>(
                ctx, transportLayerASIO->getIOContext()));
        } else if (config->serviceExecutor == "synchronous") { //同步方式
        	//构造一个链接一个线程模型对应的执行器ServiceExecutorSynchronous
            ctx->setServiceExecutor(stdx::make_unique<ServiceExecutorSynchronous>(ctx));
        }
		//transportLayerASIO转换为transportLayer类
        transportLayer = std::move(transportLayerASIO);
    } else if (serverGlobalParams.transportLayer == "legacy") {
		//获取legacy模式相关配置及初始化对应transportLayer
		transport::TransportLayerLegacy::Options opts(config);
        transportLayer = stdx::make_unique<transport::TransportLayerLegacy>(opts, sep);
        ctx->setServiceExecutor(stdx::make_unique<ServiceExecutorSynchronous>(ctx));
    }

	//transportLayer转存到对应retVector数组中并返回
    std::vector<std::unique_ptr<TransportLayer>> retVector;
    retVector.emplace_back(std::move(transportLayer));
	//构造TransportLayerManager类赋值的时候，会把retVector赋值给_tls成员，见TransportLayerManager构造函数
    return stdx::make_unique<TransportLayerManager>(std::move(retVector));
}

}  // namespace transport
}  // namespace mongo

