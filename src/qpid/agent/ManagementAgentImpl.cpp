
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
// 
//   http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//

#include "qpid/management/Manageable.h"
#include "qpid/management/ManagementObject.h"
#include "qpid/log/Statement.h"
#include "qpid/agent/ManagementAgentImpl.h"
#include "qpid/amqp_0_10/Codecs.h"
#include <list>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <iostream>
#include <fstream>
#include <boost/lexical_cast.hpp>

using namespace qpid::client;
using namespace qpid::framing;
using namespace qpid::management;
using namespace qpid::sys;
using namespace std;
using std::stringstream;
using std::ofstream;
using std::ifstream;
using std::string;
using std::endl;
using qpid::types::Variant;
using qpid::amqp_0_10::MapCodec;
using qpid::amqp_0_10::ListCodec;

namespace {
    qpid::sys::Mutex lock;
    bool disabled = false;
    ManagementAgent* agent = 0;
    int refCount = 0;
}

ManagementAgent::Singleton::Singleton(bool disableManagement)
{
    sys::Mutex::ScopedLock _lock(lock);
    if (disableManagement && !disabled) {
        disabled = true;
        assert(refCount == 0); // can't disable after agent has been allocated
    }
    if (refCount == 0 && !disabled)
        agent = new ManagementAgentImpl();
    refCount++;
}

ManagementAgent::Singleton::~Singleton()
{
    sys::Mutex::ScopedLock _lock(lock);
    refCount--;
    if (refCount == 0 && !disabled) {
        delete agent;
        agent = 0;
    }
}

ManagementAgent* ManagementAgent::Singleton::getInstance()
{
    return agent;
}

const string ManagementAgentImpl::storeMagicNumber("MA02");

ManagementAgentImpl::ManagementAgentImpl() :
    interval(10), extThread(false), pipeHandle(0), notifyCallback(0), notifyContext(0),
    notifyable(0), inCallback(false),
    initialized(false), connected(false), useMapMsg(false), lastFailure("never connected"),
    clientWasAdded(true), requestedBrokerBank(0), requestedAgentBank(0),
    assignedBrokerBank(0), assignedAgentBank(0), bootSequence(0),
    connThreadBody(*this), connThread(connThreadBody),
    pubThreadBody(*this), pubThread(pubThreadBody)
{
}

ManagementAgentImpl::~ManagementAgentImpl()
{
    // shutdown & cleanup all threads
    connThreadBody.close();
    pubThreadBody.close();

    connThread.join();
    pubThread.join();

    // Release the memory associated with stored management objects.
    {
        sys::Mutex::ScopedLock lock(agentLock);

        moveNewObjectsLH();
        for (ManagementObjectMap::iterator iter = managementObjects.begin ();
             iter != managementObjects.end ();
             iter++) {
            ManagementObject* object = iter->second;
            delete object;
        }
        managementObjects.clear();
    }
    if (pipeHandle) {
        delete pipeHandle;
        pipeHandle = 0;
    }
}

void ManagementAgentImpl::setName(const string& vendor, const string& product, const string& instance)
{
    attrMap["_vendor"] = vendor;
    attrMap["_product"] = product;
    string inst;
    if (instance.empty()) {
        inst = qpid::types::Uuid(true).str();
    } else
        inst = instance;

   name_address = vendor + ":" + product + ":" + inst;
   attrMap["_instance"] = inst;
   attrMap["_name"] = name_address;
}

void ManagementAgentImpl::init(const string& brokerHost,
                               uint16_t brokerPort,
                               uint16_t intervalSeconds,
                               bool useExternalThread,
                               const string& _storeFile,
                               const string& uid,
                               const string& pwd,
                               const string& mech,
                               const string& proto)
{
    client::ConnectionSettings settings;
    settings.protocol = proto;
    settings.host = brokerHost;
    settings.port = brokerPort;
    settings.username = uid;
    settings.password = pwd;
    settings.mechanism = mech;
    init(settings, intervalSeconds, useExternalThread, _storeFile);
}

void ManagementAgentImpl::init(const qpid::client::ConnectionSettings& settings,
                               uint16_t intervalSeconds,
                               bool useExternalThread,
                               const string& _storeFile)
{
    interval     = intervalSeconds;
    extThread    = useExternalThread;
    storeFile    = _storeFile;
    nextObjectId = 1;

    QPID_LOG(info, "QMF Agent Initialized: broker=" << settings.host << ":" << settings.port <<
             " interval=" << intervalSeconds << " storeFile=" << _storeFile);
    connectionSettings = settings;

    retrieveData();
    bootSequence++;
    if ((bootSequence & 0xF000) != 0)
        bootSequence = 1;
    storeData(true);

    if (attrMap.empty())
        setName("vendor", "product");

    initialized = true;
}

void ManagementAgentImpl::registerClass(const string& packageName,
                                        const string& className,
                                        uint8_t*     md5Sum,
                                        ManagementObject::writeSchemaCall_t schemaCall)
{ 
    sys::Mutex::ScopedLock lock(agentLock);
    PackageMap::iterator pIter = findOrAddPackage(packageName);
    addClassLocal(ManagementItem::CLASS_KIND_TABLE, pIter, className, md5Sum, schemaCall);
}

void ManagementAgentImpl::registerEvent(const string& packageName,
                                        const string& eventName,
                                        uint8_t*     md5Sum,
                                        ManagementObject::writeSchemaCall_t schemaCall)
{ 
    sys::Mutex::ScopedLock lock(agentLock);
    PackageMap::iterator pIter = findOrAddPackage(packageName);
    addClassLocal(ManagementItem::CLASS_KIND_EVENT, pIter, eventName, md5Sum, schemaCall);
}

// old-style add object: 64bit id - deprecated
ObjectId ManagementAgentImpl::addObject(ManagementObject* object,
                                        uint64_t          persistId)
{
    std::string key;
    if (persistId) {
        key = boost::lexical_cast<std::string>(persistId);
    }
    return addObject(object, key, persistId != 0);
}


// new style add object - use this approach!
ObjectId ManagementAgentImpl::addObject(ManagementObject* object,
                                        const std::string& key,
                                        bool persistent)
{
    sys::Mutex::ScopedLock lock(addLock);

    uint16_t sequence  = persistent ? 0 : bootSequence;

    ObjectId objectId(&attachment, 0, sequence);
    if (key.empty())
        objectId.setV2Key(*object);  // let object generate the key
    else
        objectId.setV2Key(key);

    object->setObjectId(objectId);
    newManagementObjects[objectId] = object;
    return objectId;
}


void ManagementAgentImpl::raiseEvent(const ManagementEvent& event, severity_t severity)
{
    sys::Mutex::ScopedLock lock(agentLock);
    Buffer outBuffer(eventBuffer, MA_BUFFER_SIZE);
    uint8_t sev = (severity == SEV_DEFAULT) ? event.getSeverity() : (uint8_t) severity;
    stringstream key;

    key << "console.event." << assignedBrokerBank << "." << assignedAgentBank << "." <<
        event.getPackageName() << "." << event.getEventName();

    Variant::Map map_;
    Variant::Map schemaId;
    Variant::Map values;
    Variant::Map headers;
    string content;

    map_["_schema_id"] = mapEncodeSchemaId(event.getPackageName(),
                                           event.getEventName(),
                                           event.getMd5Sum());
    event.mapEncode(values);
    map_["_values"] = values;
    map_["_timestamp"] = uint64_t(Duration(EPOCH, now()));
    map_["_severity"] = sev;

    headers["method"] = "indication";
    headers["qmf.opcode"] = "_data_indication";
    headers["qmf.content"] = "_event";
    headers["qmf.agent"] = name_address;

    MapCodec::encode(map_, content);
    connThreadBody.sendBuffer(content, "", headers, "qmf.default.topic", key.str());
}

uint32_t ManagementAgentImpl::pollCallbacks(uint32_t callLimit)
{
    sys::Mutex::ScopedLock lock(agentLock);

    if (inCallback) {
        QPID_LOG(critical, "pollCallbacks invoked from the agent's thread!");
        return 0;
    }

    for (uint32_t idx = 0; callLimit == 0 || idx < callLimit; idx++) {
        if (methodQueue.empty())
            break;

        QueuedMethod* item = methodQueue.front();
        methodQueue.pop_front();
        {
            sys::Mutex::ScopedUnlock unlock(agentLock);
            invokeMethodRequest(item->body, item->cid, item->replyTo);
            delete item;
        }
    }

    if (pipeHandle != 0) {
        char rbuf[100];
        while (pipeHandle->read(rbuf, 100) > 0) ; // Consume all signaling bytes
    }
    return methodQueue.size();
}

int ManagementAgentImpl::getSignalFd()
{
    if (extThread) {
        if (pipeHandle == 0)
            pipeHandle = new PipeHandle(true);
        return pipeHandle->getReadHandle();
    }

    return -1;
}

void ManagementAgentImpl::setSignalCallback(cb_t callback, void* context)
{
    sys::Mutex::ScopedLock lock(agentLock);
    notifyCallback = callback;
    notifyContext  = context;
}

void ManagementAgentImpl::setSignalCallback(Notifyable& _notifyable)
{
    sys::Mutex::ScopedLock lock(agentLock);
    notifyable = &_notifyable;
}

void ManagementAgentImpl::startProtocol()
{
    sendHeartbeat();
}

void ManagementAgentImpl::storeData(bool requested)
{
    if (!storeFile.empty()) {
        ofstream outFile(storeFile.c_str());
        uint32_t brokerBankToWrite = requested ? requestedBrokerBank : assignedBrokerBank;
        uint32_t agentBankToWrite = requested ? requestedAgentBank : assignedAgentBank;

        if (outFile.good()) {
            outFile << storeMagicNumber << " " << brokerBankToWrite << " " <<
                agentBankToWrite << " " << bootSequence << endl;
            outFile.close();
        }
    }
}

void ManagementAgentImpl::retrieveData()
{
    if (!storeFile.empty()) {
        ifstream inFile(storeFile.c_str());
        string   mn;

        if (inFile.good()) {
            inFile >> mn;
            if (mn == storeMagicNumber) {
                inFile >> requestedBrokerBank;
                inFile >> requestedAgentBank;
                inFile >> bootSequence;
            }
            inFile.close();
        }
    }
}

void ManagementAgentImpl::sendHeartbeat()
{
    static const string addr_exchange("qmf.default.topic");
    static const string addr_key("agent.ind.heartbeat");

    Variant::Map map;
    Variant::Map headers;
    string content;

    headers["method"] = "indication";
    headers["qmf.opcode"] = "_agent_heartbeat_indication";
    headers["qmf.agent"] = name_address;

    map["_values"] = attrMap;
    map["_values"].asMap()["timestamp"] = uint64_t(Duration(EPOCH, now()));
    map["_values"].asMap()["heartbeat_interval"] = interval;
    map["_values"].asMap()["epoch"] = bootSequence;

    MapCodec::encode(map, content);
    connThreadBody.sendBuffer(content, "", headers, addr_exchange, addr_key);

    QPID_LOG(trace, "SENT AgentHeartbeat name=" << name_address);
}

void ManagementAgentImpl::sendException(const string& replyToKey, const string& cid,
                                        const string& text, uint32_t code)
{
    static const string addr_exchange("qmf.default.direct");

    Variant::Map map;
    Variant::Map headers;
    Variant::Map values;
    string content;

    headers["method"] = "indication";
    headers["qmf.opcode"] = "_exception";
    headers["qmf.agent"] = name_address;

    values["error_code"] = code;
    values["error_text"] = text;
    map["_values"] = values;

    MapCodec::encode(map, content);
    connThreadBody.sendBuffer(content, cid, headers, addr_exchange, replyToKey);

    QPID_LOG(trace, "SENT Exception code=" << code <<" text=" << text);
}

void ManagementAgentImpl::handleSchemaRequest(Buffer& inBuffer, uint32_t sequence, const string& replyTo)
{
    sys::Mutex::ScopedLock lock(agentLock);
    string packageName;
    SchemaClassKey key;

    inBuffer.getShortString(packageName);
    inBuffer.getShortString(key.name);
    inBuffer.getBin128(key.hash);

    QPID_LOG(trace, "RCVD SchemaRequest: package=" << packageName << " class=" << key.name);

    PackageMap::iterator pIter = packages.find(packageName);
    if (pIter != packages.end()) {
        ClassMap& cMap = pIter->second;
        ClassMap::iterator cIter = cMap.find(key);
        if (cIter != cMap.end()) {
            SchemaClass& schema = cIter->second;
            Buffer   outBuffer(outputBuffer, MA_BUFFER_SIZE);
            uint32_t outLen;
            string   body;

            encodeHeader(outBuffer, 's', sequence);
            schema.writeSchemaCall(body);
            outBuffer.putRawData(body);
            outLen = MA_BUFFER_SIZE - outBuffer.available();
            outBuffer.reset();
            connThreadBody.sendBuffer(outBuffer, outLen, "amq.direct", replyTo);

            QPID_LOG(trace, "SENT SchemaInd: package=" << packageName << " class=" << key.name);
        }
    }
}

void ManagementAgentImpl::handleConsoleAddedIndication()
{
    sys::Mutex::ScopedLock lock(agentLock);
    clientWasAdded = true;

    QPID_LOG(trace, "RCVD ConsoleAddedInd");
}

void ManagementAgentImpl::invokeMethodRequest(const string& body, const string& cid, const string& replyTo)
{
    string  methodName;
    bool    failed = false;
    Variant::Map inMap;
    Variant::Map outMap;
    Variant::Map::const_iterator oid, mid;
    string content;

    MapCodec::decode(body, inMap);

    outMap["_values"] = Variant::Map();

    if ((oid = inMap.find("_object_id")) == inMap.end() ||
        (mid = inMap.find("_method_name")) == inMap.end()) {
        (outMap["_values"].asMap())["_status_code"] = Manageable::STATUS_PARAMETER_INVALID;
        (outMap["_values"].asMap())["_status_text"] = Manageable::StatusText(Manageable::STATUS_PARAMETER_INVALID);
        failed = true;
    } else {
        string methodName;
        ObjectId objId;
        Variant::Map inArgs;
        Variant::Map callMap;

        try {
            // conversions will throw if input is invalid.
            objId = ObjectId(oid->second.asMap());
            methodName = mid->second.getString();

            mid = inMap.find("_arguments");
            if (mid != inMap.end()) {
                inArgs = (mid->second).asMap();
            }

            ManagementObjectMap::iterator iter = managementObjects.find(objId);
            if (iter == managementObjects.end() || iter->second->isDeleted()) {
                (outMap["_values"].asMap())["_status_code"] = Manageable::STATUS_UNKNOWN_OBJECT;
                (outMap["_values"].asMap())["_status_text"] = Manageable::StatusText(Manageable::STATUS_UNKNOWN_OBJECT);
                failed = true;
            } else {
                iter->second->doMethod(methodName, inArgs, callMap);
            }

            if (callMap["_status_code"].asUint32() == 0) {
                outMap["_arguments"] = Variant::Map();
                for (Variant::Map::const_iterator iter = callMap.begin();
                     iter != callMap.end(); iter++)
                    if (iter->first != "_status_code" && iter->first != "_status_text")
                        outMap["_arguments"].asMap()[iter->first] = iter->second;
            } else {
                (outMap["_values"].asMap())["_status_code"] = callMap["_status_code"];
                (outMap["_values"].asMap())["_status_text"] = callMap["_status_text"];
                failed = true;
            }

        } catch(types::InvalidConversion& e) {
            outMap.clear();
            outMap["_values"] = Variant::Map();
            (outMap["_values"].asMap())["_status_code"] = Manageable::STATUS_EXCEPTION;
            (outMap["_values"].asMap())["_status_text"] = e.what();
            failed = true;
        }
    }

    Variant::Map headers;
    headers["method"] = "response";
    headers["qmf.agent"] = name_address;
    if (failed) {
        headers["qmf.opcode"] = "_exception";
        QPID_LOG(trace, "SENT Exception map=" << outMap);
    } else {
        headers["qmf.opcode"] = "_method_response";
        QPID_LOG(trace, "SENT MethodResponse map=" << outMap);
    }

    MapCodec::encode(outMap, content);
    connThreadBody.sendBuffer(content, cid, headers, "qmf.default.direct", replyTo);
}

void ManagementAgentImpl::handleGetQuery(const string& body, const string& cid, const string& replyTo)
{
    moveNewObjectsLH();

    Variant::Map inMap;
    Variant::Map::const_iterator i;
    Variant::Map headers;

    MapCodec::decode(body, inMap);
    QPID_LOG(trace, "RCVD GetQuery: map=" << inMap << " cid=" << cid);

    headers["method"] = "response";
    headers["qmf.opcode"] = "_query_response";
    headers["qmf.content"] = "_data";
    headers["qmf.agent"] = name_address;
    headers["partial"] = Variant();

    Variant::List list_;
    Variant::Map  map_;
    Variant::Map values;
    Variant::Map oidMap;
    string content;

    /*
     * Unpack the _what element of the query.  Currently we only support OBJECT queries.
     */
    i = inMap.find("_what");
    if (i == inMap.end()) {
        sendException(replyTo, cid, "_what element missing in Query");
        return;
    }

    if (i->second.getType() != qpid::types::VAR_STRING) {
        sendException(replyTo, cid, "_what element is not a string");
        return;
    }

    if (i->second.asString() != "OBJECT") {
        sendException(replyTo, cid, "Query for _what => '" + i->second.asString() + "' not supported");
        return;
    }

    string className;
    string packageName;

    /*
     * Handle the _schema_id element, if supplied.
     */
    i = inMap.find("_schema_id");
    if (i != inMap.end() && i->second.getType() == qpid::types::VAR_MAP) {
        const Variant::Map& schemaIdMap(i->second.asMap());

        Variant::Map::const_iterator s_iter = schemaIdMap.find("_class_name");
        if (s_iter != schemaIdMap.end() && s_iter->second.getType() == qpid::types::VAR_STRING)
            className = s_iter->second.asString();

        s_iter = schemaIdMap.find("_package_name");
        if (s_iter != schemaIdMap.end() && s_iter->second.getType() == qpid::types::VAR_STRING)
            packageName = s_iter->second.asString();
    }

    /*
     * Unpack the _object_id element of the query if it is present.  If it is present, find that one
     * object and return it.  If it is not present, send a class-based result.
     */
    i = inMap.find("_object_id");
    if (i != inMap.end() && i->second.getType() == qpid::types::VAR_MAP) {
        ObjectId objId(i->second.asMap());

        ManagementObjectMap::iterator iter = managementObjects.find(objId);
        if (iter != managementObjects.end()) {
            ManagementObject* object = iter->second;

            if (object->getConfigChanged() || object->getInstChanged())
                object->setUpdateTime();

            object->mapEncodeValues(values, true, true); // write both stats and properties
            objId.mapEncode(oidMap);
            map_["_values"] = values;
            map_["_object_id"] = oidMap;
            object->writeTimestamps(map_);
            map_["_schema_id"] = mapEncodeSchemaId(object->getPackageName(),
                                                   object->getClassName(),
                                                   object->getMd5Sum());

            list_.push_back(map_);
            headers.erase("partial");

            ListCodec::encode(list_, content);
            connThreadBody.sendBuffer(content, cid, headers, "qmf.default.direct", replyTo, "amqp/list");
            QPID_LOG(trace, "SENT QueryResponse (query by object_id) to=" << replyTo);
            return;
        }
    } else {
        for (ManagementObjectMap::iterator iter = managementObjects.begin();
             iter != managementObjects.end();
             iter++) {
            ManagementObject* object = iter->second;
            if (object->getClassName() == className &&
                (packageName.empty() || object->getPackageName() == packageName)) {

                // @todo support multiple object reply per message
                values.clear();
                list_.clear();
                oidMap.clear();

                if (object->getConfigChanged() || object->getInstChanged())
                    object->setUpdateTime();

                object->mapEncodeValues(values, true, true); // write both stats and properties
                iter->first.mapEncode(oidMap);
                map_["_values"] = values;
                map_["_object_id"] = oidMap;
                object->writeTimestamps(map_);
                map_["_schema_id"] = mapEncodeSchemaId(object->getPackageName(),
                                                       object->getClassName(),
                                                       object->getMd5Sum());
                list_.push_back(map_);

                ListCodec::encode(list_, content);
                connThreadBody.sendBuffer(content, cid, headers, "qmf.default.direct", replyTo, "amqp/list");
                QPID_LOG(trace, "SENT QueryResponse (query by schema_id) to=" << replyTo);
            }
        }
    }

    // end empty "non-partial" message to indicate CommandComplete
    list_.clear();
    headers.erase("partial");
    ListCodec::encode(list_, content);
    connThreadBody.sendBuffer(content, cid, headers, "qmf.default.direct", replyTo, "amqp/list");
    QPID_LOG(trace, "SENT QueryResponse (empty with no 'partial' indicator) to=" << replyTo);
}

void ManagementAgentImpl::handleLocateRequest(const string&, const string& cid, const string& replyTo)
{
    QPID_LOG(trace, "RCVD AgentLocateRequest");
    static const string addr_exchange("qmf.default.direct");

    Variant::Map map;
    Variant::Map headers;
    string content;

    headers["method"] = "indication";
    headers["qmf.opcode"] = "_agent_locate_response";
    headers["qmf.agent"] = name_address;

    map["_values"] = attrMap;
    map["_values"].asMap()["timestamp"] = uint64_t(Duration(EPOCH, now()));
    map["_values"].asMap()["heartbeat_interval"] = interval;
    map["_values"].asMap()["epoch"] = bootSequence;

    MapCodec::encode(map, content);
    connThreadBody.sendBuffer(content, cid, headers, addr_exchange, replyTo);

    QPID_LOG(trace, "SENT AgentLocateResponse replyTo=" << replyTo);

    {
        sys::Mutex::ScopedLock lock(agentLock);
        clientWasAdded = true;
    }
}

void ManagementAgentImpl::handleMethodRequest(const string& body, const string& cid, const string& replyTo)
{
    if (extThread) {
        sys::Mutex::ScopedLock lock(agentLock);

        methodQueue.push_back(new QueuedMethod(cid, replyTo, body));
        if (pipeHandle != 0) {
            pipeHandle->write("X", 1);
        } else if (notifyable != 0) {
            inCallback = true;
            {
                sys::Mutex::ScopedUnlock unlock(agentLock);
                notifyable->notify();
            }
            inCallback = false;
        } else if (notifyCallback != 0) {
            inCallback = true;
            {
                sys::Mutex::ScopedUnlock unlock(agentLock);
                notifyCallback(notifyContext);
            }
            inCallback = false;
        }
    } else {
        invokeMethodRequest(body, cid, replyTo);
    }

    QPID_LOG(trace, "RCVD MethodRequest");
}

void ManagementAgentImpl::received(Message& msg)
{
    string   replyToKey;
    framing::MessageProperties mp = msg.getMessageProperties();
    if (mp.hasReplyTo()) {
        const framing::ReplyTo& rt = mp.getReplyTo();
        replyToKey = rt.getRoutingKey();
    }

    if (mp.hasAppId() && mp.getAppId() == "qmf2")
    {
        string opcode = mp.getApplicationHeaders().getAsString("qmf.opcode");
        string cid = msg.getMessageProperties().getCorrelationId();

        if      (opcode == "_agent_locate_request") handleLocateRequest(msg.getData(), cid, replyToKey);
        else if (opcode == "_method_request")       handleMethodRequest(msg.getData(), cid, replyToKey);
        else if (opcode == "_query_request")        handleGetQuery(msg.getData(), cid, replyToKey);
        else {
            QPID_LOG(warning, "Support for QMF V2 Opcode [" << opcode << "] TBD!!!");
        }
        return;
    }

    // old preV2 binary messages
    
    uint32_t sequence;
    string   data = msg.getData();
    Buffer   inBuffer(const_cast<char*>(data.c_str()), data.size());
    uint8_t  opcode;


    if (checkHeader(inBuffer, &opcode, &sequence))
    {
        if      (opcode == 'S') handleSchemaRequest(inBuffer, sequence, replyToKey);
        else if (opcode == 'x') handleConsoleAddedIndication();
        else
            QPID_LOG(warning, "Ignoring old-format QMF Request! opcode=" << char(opcode));
    }
}


void ManagementAgentImpl::encodeHeader(Buffer& buf, uint8_t opcode, uint32_t seq)
{
    buf.putOctet('A');
    buf.putOctet('M');
    buf.putOctet('2');
    buf.putOctet(opcode);
    buf.putLong (seq);
}

Variant::Map ManagementAgentImpl::mapEncodeSchemaId(const string& pname,
                                                    const string& cname,
                                                    const uint8_t *md5Sum)
{
    Variant::Map map_;

    map_["_package_name"] = pname;
    map_["_class_name"] = cname;
    map_["_hash"] = types::Uuid(md5Sum);
    return map_;
}


bool ManagementAgentImpl::checkHeader(Buffer& buf, uint8_t *opcode, uint32_t *seq)
{
    if (buf.getSize() < 8)
        return false;

    uint8_t h1 = buf.getOctet();
    uint8_t h2 = buf.getOctet();
    uint8_t h3 = buf.getOctet();

    *opcode = buf.getOctet();
    *seq    = buf.getLong();

    return h1 == 'A' && h2 == 'M' && h3 == '2';
}

ManagementAgentImpl::PackageMap::iterator ManagementAgentImpl::findOrAddPackage(const string& name)
{
    PackageMap::iterator pIter = packages.find(name);
    if (pIter != packages.end())
        return pIter;

    // No such package found, create a new map entry.
    pair<PackageMap::iterator, bool> result =
        packages.insert(pair<string, ClassMap>(name, ClassMap()));

    if (connected) {
        // Publish a package-indication message
        Buffer   outBuffer(outputBuffer, MA_BUFFER_SIZE);
        uint32_t outLen;

        encodeHeader(outBuffer, 'p');
        encodePackageIndication(outBuffer, result.first);
        outLen = MA_BUFFER_SIZE - outBuffer.available();
        outBuffer.reset();
        connThreadBody.sendBuffer(outBuffer, outLen, "qpid.management", "schema.package");
    }

    return result.first;
}

void ManagementAgentImpl::moveNewObjectsLH()
{
    sys::Mutex::ScopedLock lock(addLock);
    for (ManagementObjectMap::iterator iter = newManagementObjects.begin();
         iter != newManagementObjects.end();
         iter++)
        managementObjects[iter->first] = iter->second;
    newManagementObjects.clear();
}

void ManagementAgentImpl::addClassLocal(uint8_t               classKind,
                                        PackageMap::iterator  pIter,
                                        const string&         className,
                                        uint8_t*              md5Sum,
                                        ManagementObject::writeSchemaCall_t schemaCall)
{
    SchemaClassKey key;
    ClassMap&      cMap = pIter->second;

    key.name = className;
    memcpy(&key.hash, md5Sum, 16);

    ClassMap::iterator cIter = cMap.find(key);
    if (cIter != cMap.end())
        return;

    // No such class found, create a new class with local information.
    cMap.insert(pair<SchemaClassKey, SchemaClass>(key, SchemaClass(schemaCall, classKind)));
}

void ManagementAgentImpl::encodePackageIndication(Buffer&              buf,
                                                  PackageMap::iterator pIter)
{
    buf.putShortString((*pIter).first);

    QPID_LOG(trace, "SENT PackageInd: package=" << (*pIter).first);
}

void ManagementAgentImpl::encodeClassIndication(Buffer&              buf,
                                                PackageMap::iterator pIter,
                                                ClassMap::iterator   cIter)
{
    SchemaClassKey key = (*cIter).first;

    buf.putOctet((*cIter).second.kind);
    buf.putShortString((*pIter).first);
    buf.putShortString(key.name);
    buf.putBin128(key.hash);

    QPID_LOG(trace, "SENT ClassInd: package=" << (*pIter).first << " class=" << key.name);
}

void ManagementAgentImpl::periodicProcessing()
{
    sys::Mutex::ScopedLock lock(agentLock);
    list<pair<ObjectId, ManagementObject*> > deleteList;

    if (!connected)
        return;

    moveNewObjectsLH();

    //
    //  Clear the been-here flag on all objects in the map.
    //
    for (ManagementObjectMap::iterator iter = managementObjects.begin();
         iter != managementObjects.end();
         iter++) {
        ManagementObject* object = iter->second;
        object->setFlags(0);
        if (clientWasAdded) {
            object->setForcePublish(true);
        }
    }

    clientWasAdded = false;

    //
    //  Process the entire object map.
    //
    for (ManagementObjectMap::iterator baseIter = managementObjects.begin();
         baseIter != managementObjects.end();
         baseIter++) {
        ManagementObject* baseObject = baseIter->second;

        //
        //  Skip until we find a base object requiring a sent message.
        //
        if (baseObject->getFlags() == 1 ||
            (!baseObject->getConfigChanged() &&
             !baseObject->getInstChanged() &&
             !baseObject->getForcePublish() &&
             !baseObject->isDeleted()))
            continue;

        Variant::List list_;

        for (ManagementObjectMap::iterator iter = baseIter;
             iter != managementObjects.end();
             iter++) {
            ManagementObject* object = iter->second;
            bool send_stats, send_props;
            if (baseObject->isSameClass(*object) && object->getFlags() == 0) {
                object->setFlags(1);
                if (object->getConfigChanged() || object->getInstChanged())
                    object->setUpdateTime();

                send_props = (object->getConfigChanged() || object->getForcePublish() || object->isDeleted());
                send_stats = (object->hasInst() && (object->getInstChanged() || object->getForcePublish()));

                if (send_stats || send_props) {
                    Variant::Map map_;
                    Variant::Map values;
                    Variant::Map oid;

                    object->getObjectId().mapEncode(oid);
                    map_["_object_id"] = oid;
                    map_["_schema_id"] = mapEncodeSchemaId(object->getPackageName(),
                                                           object->getClassName(),
                                                           object->getMd5Sum());
                    object->writeTimestamps(map_);
                    object->mapEncodeValues(values, send_props, send_stats);
                    map_["_values"] = values;
                    list_.push_back(map_);
                }

                if (object->isDeleted())
                    deleteList.push_back(pair<ObjectId, ManagementObject*>(iter->first, object));
                object->setForcePublish(false);
            }
        }

        string content;
        ListCodec::encode(list_, content);
        if (content.length()) {
            Variant::Map  headers;
            headers["method"] = "indication";
            headers["qmf.opcode"] = "_data_indication";
            headers["qmf.content"] = "_data";
            headers["qmf.agent"] = name_address;

            connThreadBody.sendBuffer(content, "", headers, "qmf.default.topic", "agent.ind.data", "amqp/list");
            QPID_LOG(trace, "SENT DataIndication");
        }
    }

    // Delete flagged objects
    for (list<pair<ObjectId, ManagementObject*> >::reverse_iterator iter = deleteList.rbegin();
         iter != deleteList.rend();
         iter++) {
        delete iter->second;
        managementObjects.erase(iter->first);
    }

    deleteList.clear();
    sendHeartbeat();
}

void ManagementAgentImpl::ConnectionThread::run()
{
    static const int delayMin(1);
    static const int delayMax(128);
    static const int delayFactor(2);
    int delay(delayMin);
    string dest("qmfagent");
    ConnectionThread::shared_ptr tmp;

    sessionId.generate();
    queueName << "qmfagent-" << sessionId;

    while (true) {
        try {
            if (agent.initialized) {
                QPID_LOG(debug, "QMF Agent attempting to connect to the broker...");
                connection.open(agent.connectionSettings);
                session = connection.newSession(queueName.str());
                subscriptions.reset(new client::SubscriptionManager(session));

                session.queueDeclare(arg::queue=queueName.str(), arg::autoDelete=true,
                                     arg::exclusive=true);
                session.exchangeBind(arg::exchange="amq.direct", arg::queue=queueName.str(),
                                     arg::bindingKey=queueName.str());
                session.exchangeBind(arg::exchange="qmf.default.direct", arg::queue=queueName.str(),
                                     arg::bindingKey=agent.name_address);
                session.exchangeBind(arg::exchange="qmf.default.topic", arg::queue=queueName.str(),
                                     arg::bindingKey="console.#");

                subscriptions->subscribe(agent, queueName.str(), dest);
                QPID_LOG(info, "Connection established with broker");
                {
                    sys::Mutex::ScopedLock _lock(connLock);
                    if (shutdown)
                        return;
                    operational = true;
                    agent.connected = true;
                    agent.startProtocol();
                    try {
                        sys::Mutex::ScopedUnlock _unlock(connLock);
                        subscriptions->run();
                    } catch (exception) {}

                    QPID_LOG(warning, "Connection to the broker has been lost");

                    operational = false;
                    agent.connected = false;
                    tmp = subscriptions;
                    subscriptions.reset();
                }
                tmp.reset();    // frees the subscription outside the lock
                delay = delayMin;
                connection.close();
            }
        } catch (exception &e) {
            if (delay < delayMax)
                delay *= delayFactor;
            QPID_LOG(debug, "Connection failed: exception=" << e.what());
        }

        {
            // sleep for "delay" seconds, but peridically check if the
            // agent is shutting down so we don't hang for up to delayMax 
            // seconds during agent shutdown
             sys::Mutex::ScopedLock _lock(connLock);
             if (shutdown)
                 return;
             sleeping = true;
             int totalSleep = 0;
             do {
                 sys::Mutex::ScopedUnlock _unlock(connLock);
                 ::sleep(delayMin);
                 totalSleep += delayMin;
             } while (totalSleep < delay && !shutdown);
             sleeping = false;
             if (shutdown)
                 return;
        }
    }
}

ManagementAgentImpl::ConnectionThread::~ConnectionThread()
{
}

void ManagementAgentImpl::ConnectionThread::sendBuffer(Buffer&  buf,
                                                       uint32_t length,
                                                       const string& exchange,
                                                       const string& routingKey)
{
    Message msg;
    string  data;

    buf.getRawData(data, length);
    msg.setData(data);
    sendMessage(msg, exchange, routingKey);
}



void ManagementAgentImpl::ConnectionThread::sendBuffer(const string& data,
                                                       const string& cid,
                                                       const Variant::Map headers,
                                                       const string& exchange,
                                                       const string& routingKey,
                                                       const string& contentType)
{
    Message msg;
    Variant::Map::const_iterator i;

    if (!cid.empty())
        msg.getMessageProperties().setCorrelationId(cid);

    if (!contentType.empty())
        msg.getMessageProperties().setContentType(contentType);
    for (i = headers.begin(); i != headers.end(); ++i) {
        msg.getHeaders().setString(i->first, i->second.asString());
    }
    msg.getHeaders().setString("app_id", "qmf2");

    msg.setData(data);
    sendMessage(msg, exchange, routingKey);
}





void ManagementAgentImpl::ConnectionThread::sendMessage(Message msg,
                                                        const string& exchange,
                                                        const string& routingKey)
{
    ConnectionThread::shared_ptr s;
    {
        sys::Mutex::ScopedLock _lock(connLock);
        if (!operational)
            return;
        s = subscriptions;
    }

    msg.getDeliveryProperties().setRoutingKey(routingKey);
    msg.getMessageProperties().setReplyTo(ReplyTo("amq.direct", queueName.str()));
    msg.getMessageProperties().getApplicationHeaders().setString("qmf.agent", agent.name_address);
    try {
        session.messageTransfer(arg::content=msg, arg::destination=exchange);
    } catch(exception& e) {
        QPID_LOG(error, "Exception caught in sendMessage: " << e.what());
        // Bounce the connection
        if (s)
            s->stop();
    }
}



void ManagementAgentImpl::ConnectionThread::bindToBank(uint32_t brokerBank, uint32_t agentBank)
{
    stringstream key;
    key << "agent." << brokerBank << "." << agentBank;
    session.exchangeBind(arg::exchange="qpid.management", arg::queue=queueName.str(),
                         arg::bindingKey=key.str());
}

void ManagementAgentImpl::ConnectionThread::close()
{
    ConnectionThread::shared_ptr s;
    {
        sys::Mutex::ScopedLock _lock(connLock);
        shutdown = true;
        s = subscriptions;
    }
    if (s)
        s->stop();
}

bool ManagementAgentImpl::ConnectionThread::isSleeping() const
{
    sys::Mutex::ScopedLock _lock(connLock);
    return sleeping;
}


void ManagementAgentImpl::PublishThread::run()
{
    uint16_t    totalSleep;

    while (!shutdown) {
        agent.periodicProcessing();
        totalSleep = 0;
        while (totalSleep++ < agent.getInterval() && !shutdown) {
            ::sleep(1);
        }
    }
}
