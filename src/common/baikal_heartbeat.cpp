// Copyright (c) 2018-present Baidu, Inc. All Rights Reserved.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "baikal_heartbeat.h"
#include "schema_factory.h"
#include "task_fetcher.h"
namespace baikaldb {

void BaikalHeartBeat::construct_heart_beat_request(pb::BaikalHeartBeatRequest& request, bool is_backup) {
    SchemaFactory* factory = nullptr;
    if (is_backup) {
        factory = SchemaFactory::get_backup_instance();
    } else {
        factory = SchemaFactory::get_instance();
    }
    if (!factory->is_inited()) {
        return;
    }

    auto schema_read_recallback = [&request, factory](const SchemaMapping& schema){
        auto& table_statistics_mapping = schema.table_statistics_mapping;
        for (auto& info_pair : schema.table_info_mapping) {
            if (info_pair.second->engine != pb::ROCKSDB &&
                    info_pair.second->engine != pb::ROCKSDB_CSTORE && 
                    info_pair.second->engine != pb::BINLOG) {
                continue;
            }
            //主键索引和全局二级索引都需要传递region信息
            for (auto& index_id : info_pair.second->indices) {
                auto& index_info_mapping = schema.index_info_mapping;
                if (index_info_mapping.count(index_id) == 0) {
                    continue;
                }
                IndexInfo index = *index_info_mapping.at(index_id);
                //主键索引
                if (index.type != pb::I_PRIMARY && !index.is_global) {
                    continue;
                }
                auto req_info = request.add_schema_infos();
                req_info->set_table_id(index_id);
                req_info->set_version(1);
                int64_t version = 0;
                auto iter = table_statistics_mapping.find(index_id);
                if (iter != table_statistics_mapping.end()) {
                    version = iter->second->version();
                }
                req_info->set_statis_version(version);

                if (index_id == info_pair.second->id) {
                    req_info->set_version(info_pair.second->version);
                }
            }  
        }
    };
    request.set_last_updated_index(factory->last_updated_index());
    factory->schema_info_scope_read(schema_read_recallback);
    TaskFactory<pb::RegionDdlWork>::get_instance()->construct_heartbeat(request, 
        &pb::BaikalHeartBeatRequest::add_region_ddl_works);
    TaskFactory<pb::DdlWorkInfo>::get_instance()->construct_heartbeat(request, 
        &pb::BaikalHeartBeatRequest::add_ddl_works);
    request.set_can_do_ddlwork(is_backup ? false : true);
    //定时将request当中消息体更新 等待process_baikal_heartbeat处理更新至TableManager中的virtual_Index_sql_map
    auto sample = factory->get_virtual_index_info();
    auto index_id_name_map = sample.index_id_name_map;
    auto index_id_sample_sqls_map = sample.index_id_sample_sqls_map;
    //遍历map 更新至request 
    for (auto& it1 : index_id_name_map) {
        for (auto& it2 : index_id_sample_sqls_map[it1.first]) {
            pb::VirtualIndexInfluence virtual_index_influence;
            virtual_index_influence.set_virtual_index_id(it1.first);
            virtual_index_influence.set_virtual_index_name(it1.second);
            virtual_index_influence.set_influenced_sql(it2);
            pb::VirtualIndexInfluence* info_affect = request.add_info_affect();
            *info_affect = virtual_index_influence ;
        }
    }
}

void BaikalHeartBeat::process_heart_beat_response(const pb::BaikalHeartBeatResponse& response, bool is_backup) {
    SchemaFactory* factory = nullptr;
    if (is_backup) {
        factory = SchemaFactory::get_backup_instance();
    } else {
        factory = SchemaFactory::get_instance();
    }
    if (!factory->is_inited()) {
        return;
    }

    for (auto& info : response.schema_change_info()) {
        factory->update_table(info);
    }
    factory->update_regions(response.region_change_info());
    if (response.has_idc_info()) {
        factory->update_idc(response.idc_info());
    }
    for (auto& info : response.privilege_change_info()) {
        factory->update_user(info);
    }
    factory->update_show_db(response.db_info());
    if (response.statistics().size() > 0) {
        factory->update_statistics(response.statistics());
    }
    if (response.has_last_updated_index() && 
        response.last_updated_index() > factory->last_updated_index()) {
        factory->set_last_updated_index(response.last_updated_index());
    }
    
    TaskFactory<pb::RegionDdlWork>::get_instance()->process_heartbeat(response, 
        static_cast<
            const google::protobuf::RepeatedPtrField<pb::RegionDdlWork>& (pb::BaikalHeartBeatResponse::*)() const
        >(&pb::BaikalHeartBeatResponse::region_ddl_works));
    TaskFactory<pb::DdlWorkInfo>::get_instance()->process_heartbeat(response, 
        static_cast<
            const google::protobuf::RepeatedPtrField<pb::DdlWorkInfo>& (pb::BaikalHeartBeatResponse::*)() const
        >(&pb::BaikalHeartBeatResponse::ddl_works));
}

void BaikalHeartBeat::process_heart_beat_response_sync(const pb::BaikalHeartBeatResponse& response) {
    TimeCost cost;
    SchemaFactory* factory = SchemaFactory::get_instance();
    factory->update_tables_double_buffer_sync(response.schema_change_info());

    if (response.has_idc_info()) {
        factory->update_idc(response.idc_info());
    }
    for (auto& info : response.privilege_change_info()) {
        factory->update_user(info);
    }
    factory->update_show_db(response.db_info());
    if (response.statistics().size() > 0) {
        factory->update_statistics(response.statistics());
    }

    factory->update_regions_double_buffer_sync(response.region_change_info());
    if (response.has_last_updated_index() && 
        response.last_updated_index() > factory->last_updated_index()) {
        factory->set_last_updated_index(response.last_updated_index());
    }
    DB_NOTICE("sync time:%ld", cost.get_time());
}

//binlog network
bool BinlogNetworkServer::init() {
    // init val 
    TimeCost cost;
    // 先把meta数据都获取到
    pb::BaikalHeartBeatRequest request;
    pb::BaikalHeartBeatResponse response;
    //1、构造心跳请求
   BaikalHeartBeat::construct_heart_beat_request(request);
   request.set_can_do_ddlwork(false);
    //2、发送请求
    if (MetaServerInteract::get_instance()->send_request("baikal_heartbeat", request, response) == 0) {
        //处理心跳
        return process_heart_beat_response_sync(response);
        //DB_WARNING("req:%s  \nres:%s", request.DebugString().c_str(), response.DebugString().c_str());
    } else {
        DB_FATAL("send heart beat request to meta server fail");
        return false;
    }

    return true;
}

void BinlogNetworkServer::report_heart_beat() {
    while (!_shutdown) {
        TimeCost cost;
        pb::BaikalHeartBeatRequest request;
        pb::BaikalHeartBeatResponse response;
        //1、construct heartbeat request
        BaikalHeartBeat::construct_heart_beat_request(request);
        int64_t construct_req_cost = cost.get_time();
        cost.reset();
        //2、send heartbeat request to meta server
        if (MetaServerInteract::get_instance()->send_request("baikal_heartbeat", request, response) == 0) {
            //处理心跳
            process_heart_beat_response(response);
            DB_WARNING("report_heart_beat, construct_req_cost:%ld, process_res_cost:%ld",
                    construct_req_cost, cost.get_time());
        } else {
            DB_WARNING("send heart beat request to meta server fail");
        }
        bthread_usleep_fast_shutdown(1000 * 1000 * 10, _shutdown);
    }
}

void BinlogNetworkServer::process_heart_beat_response(const pb::BaikalHeartBeatResponse& response) {
    DB_DEBUG("response %s", response.ShortDebugString().c_str());
    SchemaFactory* factory = SchemaFactory::get_instance();
    
    for (auto& info : response.schema_change_info()) {
        factory->update_table(info);
    }
    RegionVec rv;
    auto& region_change_info = response.region_change_info();
    for (auto& region_info : region_change_info) {
        if (_binlog_id != region_info.table_id()) {
            DB_WARNING("skip region info table_id %ld", region_info.table_id());
            continue;
        }
        *rv.Add() = region_info;
    }
    factory->update_regions(rv);
    if (response.has_idc_info()) {
        factory->update_idc(response.idc_info());
    }
    for (auto& info : response.privilege_change_info()) {
        factory->update_user(info);
    }
    factory->update_show_db(response.db_info());
    if (response.statistics().size() > 0) {
        factory->update_statistics(response.statistics());
    }
    if (response.has_last_updated_index() && 
        response.last_updated_index() > factory->last_updated_index()) {
        factory->set_last_updated_index(response.last_updated_index());
    }
}

bool BinlogNetworkServer::process_heart_beat_response_sync(const pb::BaikalHeartBeatResponse& response) {
    DB_DEBUG("response %s", response.ShortDebugString().c_str());
    TimeCost cost;
    SchemaFactory* factory = SchemaFactory::get_instance();
    SchemaVec sv;
    std::vector<int64_t> tmp_tids;
    tmp_tids.reserve(10);
    factory->update_tables_double_buffer_sync(response.schema_change_info());

    for (const auto& table_name : _table_names) {
        int64_t tid;
        if (factory->get_table_id(table_name, tid) != 0) {
            DB_FATAL("get table[%s] id error", table_name.c_str());
        }
        DB_NOTICE("get table_name[%s] table_id[%ld]", table_name.c_str(), tid);
        _binlog_table_ids.insert(tid);
    }
    //获取binlog table_id
    int64_t binlog_id = 0;
    for (auto tid : _binlog_table_ids) {
        if (factory->get_binlog_id(tid, binlog_id) != 0) {
            DB_FATAL("config binlog error. %ld", tid);
            continue;
        }
        DB_NOTICE("insert binlog table id %ld", binlog_id);
        if (_binlog_id != -1) {
            if (binlog_id != _binlog_id) {
                DB_FATAL("tables has different binlog id %ld", tid);
                continue;
            }
        } else {
            _binlog_id = binlog_id;
        }
    }
    if (_binlog_id == -1) {
        DB_FATAL("get binlog id error.");
        return false;
    }

    if (response.has_idc_info()) {
        factory->update_idc(response.idc_info());
    }
    for (auto& info : response.privilege_change_info()) {
        factory->update_user(info);
    }
    RegionVec rv;
    auto& region_change_info = response.region_change_info();
    for (auto& region_info : region_change_info) {
        if (_binlog_id != region_info.table_id()) {
            DB_NOTICE("skip region info table_id %ld", region_info.table_id());
            continue;
        }
        *rv.Add() = region_info;
    }
    factory->update_regions(rv);
    factory->update_show_db(response.db_info());
    if (response.statistics().size() > 0) {
        factory->update_statistics(response.statistics());
    }

    if (response.has_last_updated_index() && 
        response.last_updated_index() > factory->last_updated_index()) {
        factory->set_last_updated_index(response.last_updated_index());
    }
    DB_NOTICE("sync time:%ld", cost.get_time());
    return true;
}
}
