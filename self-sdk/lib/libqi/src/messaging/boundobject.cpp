/*
**  Copyright (C) 2012 Aldebaran Robotics
**  See COPYING for the license
*/

#include <boost/make_shared.hpp>

#include <qi/anyobject.hpp>
#include <qi/type/objecttypebuilder.hpp>
#include "boundobject.hpp"

qiLogCategory("qimessaging.boundobject");

namespace qi {

  static AnyReference forwardEvent(const GenericFunctionParameters& params,
                                   unsigned int service, unsigned int object,
                                   unsigned int event, Signature sig,
                                   TransportSocketPtr client,
                                   ObjectHost* context,
                                   const std::string& signature)
  {
    qiLogDebug() << "forwardEvent";
    qi::Message msg;
    // FIXME: would like to factor with serveresult.hpp convertAndSetValue()
    // but we have a setValue/setValues issue
    bool processed = false;
    if (!signature.empty() && client->remoteCapability("MessageFlags", false))
    {
      qiLogDebug() << "forwardEvent attempting conversion to " << signature;
      try
      {
        GenericFunctionParameters res = params.convert(signature);
        // invalid conversion does not throw it seems
        bool valid = true;
        for (unsigned i=0; i<res.size(); ++i)
        {
          if (!res[i].type())
          {
            valid = false;
            break;
          }
        }
        if (valid)
        {
          qiLogDebug() << "forwardEvent success " << res[0].type()->infoString();
          msg.setValues(res, "m", context, client.get());
          msg.addFlags(Message::TypeFlag_DynamicPayload);
          res.destroy();
          processed = true;
        }
      }
      catch(const std::exception& )
      {
        qiLogDebug() << "forwardEvent failed to convert to forced type";
      }
    }
    if (!processed)
    {
      try {
        msg.setValues(params, sig, context, client.get());
      }
      catch (const std::exception& e)
      {
        qiLogVerbose() << "forwardEvent::setValues exception: " << e.what();
        if (!client->remoteCapability("MessageFlags", false))
          throw e;
        // Delegate conversion to the remote end.
        msg.addFlags(Message::TypeFlag_DynamicPayload);
        msg.setValues(params, "m", context, client.get());
      }
    }
    msg.setService(service);
    msg.setFunction(event);
    msg.setType(Message::Type_Event);
    msg.setObject(object);
    client->send(msg);
    return AnyReference();
  }

  struct ServiceBoundObject::CancelableKit
  {
    ServiceBoundObject::CancelableMap map;
    boost::mutex                      guard;
  };

  ServiceBoundObject::ServiceBoundObject(unsigned int serviceId, unsigned int objectId,
                                         qi::AnyObject object,
                                         qi::MetaCallType mct,
                                         bool bindTerminate,
                                         ObjectHost* owner)
    : ObjectHost(serviceId)
    , _cancelables(boost::make_shared<CancelableKit>())
    , _links()
    , _serviceId(serviceId)
    , _objectId(objectId)
    , _object(object)
    , _callType(mct)
    , _owner(owner)
  {
    onDestroy.setCallType(MetaCallType_Direct);
    _self = createServiceBoundObjectType(this, bindTerminate);
  }

  ServiceBoundObject::~ServiceBoundObject()
  {
    qiLogDebug() << "~ServiceBoundObject()";
    _cancelables.reset();
    ObjectHost::clear();
    if (_owner)
      _owner->removeObject(_objectId);
    onDestroy(this);
    qiLogDebug() << "~ServiceBoundObject() reseting object " << _object.use_count();
    _object.reset();
    qiLogDebug() << "~ServiceBoundObject() finishing";
  }

  qi::AnyObject ServiceBoundObject::createServiceBoundObjectType(ServiceBoundObject *self, bool bindTerminate) {
    static qi::ObjectTypeBuilder<ServiceBoundObject>* ob = 0;

    static boost::mutex* mutex = 0;
    QI_THREADSAFE_NEW(mutex);
    boost::mutex::scoped_lock lock(*mutex);
    if (!ob)
    {
      ob = new qi::ObjectTypeBuilder<ServiceBoundObject>();
      // these are called synchronously by onMessage (and this is needed for
      // _currentSocket), no need for threadsafety here
      ob->setThreadingModel(ObjectThreadingModel_MultiThread);
      /* Network-related stuff.
      */
      ob->advertiseMethod("registerEvent"  , &ServiceBoundObject::registerEvent, MetaCallType_Direct, qi::Message::BoundObjectFunction_RegisterEvent);
      ob->advertiseMethod("unregisterEvent", &ServiceBoundObject::unregisterEvent, MetaCallType_Direct, qi::Message::BoundObjectFunction_UnregisterEvent);
      ob->advertiseMethod("terminate",       &ServiceBoundObject::terminate, MetaCallType_Direct, qi::Message::BoundObjectFunction_Terminate);
      /* GenericObject-related stuff.
      * Those methods could be advertised and implemented by GenericObject itself.
      * But since we already have a wrapper system in place in BoundObject, us it.
      * There is no use-case that requires the methods below without a BoundObject present.
      */
      ob->advertiseMethod("metaObject"     , &ServiceBoundObject::metaObject, MetaCallType_Direct, qi::Message::BoundObjectFunction_MetaObject);
      ob->advertiseMethod("property",       &ServiceBoundObject::property, MetaCallType_Direct, qi::Message::BoundObjectFunction_GetProperty);
      ob->advertiseMethod("setProperty",       &ServiceBoundObject::setProperty, MetaCallType_Direct, qi::Message::BoundObjectFunction_SetProperty);
      ob->advertiseMethod("properties",       &ServiceBoundObject::properties, MetaCallType_Direct, qi::Message::BoundObjectFunction_Properties);
      ob->advertiseMethod("registerEventWithSignature"  , &ServiceBoundObject::registerEventWithSignature, MetaCallType_Direct, qi::Message::BoundObjectFunction_RegisterEventWithSignature);
    }
    AnyObject result = ob->object(self, &AnyObject::deleteGenericObjectOnly);
    return result;
  }

  //Bound Method
  SignalLink ServiceBoundObject::registerEvent(unsigned int objectId, unsigned int eventId, SignalLink remoteSignalLinkId) {
    // fetch signature
    const MetaSignal* ms = _object.metaObject().signal(eventId);
    if (!ms)
      throw std::runtime_error("No such signal");
    assert(_currentSocket);
    AnyFunction mc = AnyFunction::fromDynamicFunction(boost::bind(&forwardEvent, _1, _serviceId, _objectId, eventId, ms->parametersSignature(), _currentSocket, this, ""));
    SignalLink linkId = _object.connect(eventId, mc);
    qiLogDebug() << "SBO rl " << remoteSignalLinkId <<" ll " << linkId;
    _links[_currentSocket][remoteSignalLinkId] = RemoteSignalLink(linkId, eventId);
    return linkId;
  }
  SignalLink ServiceBoundObject::registerEventWithSignature(unsigned int objectId, unsigned int eventId, SignalLink remoteSignalLinkId, const std::string& signature) {
    // fetch signature
    const MetaSignal* ms = _object.metaObject().signal(eventId);
    if (!ms)
      throw std::runtime_error("No such signal");
    assert(_currentSocket);
    AnyFunction mc = AnyFunction::fromDynamicFunction(boost::bind(&forwardEvent, _1, _serviceId, _objectId, eventId, ms->parametersSignature(), _currentSocket, this, signature));
    SignalLink linkId = _object.connect(eventId, mc);
    qiLogDebug() << "SBO rl " << remoteSignalLinkId <<" ll " << linkId;
    _links[_currentSocket][remoteSignalLinkId] = RemoteSignalLink(linkId, eventId);
    return linkId;
  }

  //Bound Method
  void ServiceBoundObject::unregisterEvent(unsigned int objectId, unsigned int QI_UNUSED(event), SignalLink remoteSignalLinkId) {
    ServiceSignalLinks&          sl = _links[_currentSocket];
    ServiceSignalLinks::iterator it = sl.find(remoteSignalLinkId);

    if (it == sl.end())
    {
      std::stringstream ss;
      ss << "Unregister request failed for " << remoteSignalLinkId <<" " << objectId;
      qiLogError() << ss.str();
      throw std::runtime_error(ss.str());
    }
    _object.disconnect(it->second.localSignalLinkId);
    sl.erase(it);
    if (sl.empty())
      _links.erase(_currentSocket);
  }

  //Bound Method
  qi::MetaObject ServiceBoundObject::metaObject(unsigned int objectId) {
    //we inject specials methods here
    return qi::MetaObject::merge(_self.metaObject(), _object.metaObject());
  }


  void ServiceBoundObject::terminate(unsigned int)
  {
    qiLogDebug() << "terminate() received";
    if (_owner)
      _owner->removeObject(_objectId);
    else
      qiLogWarning() << "terminate() received on object without owner";
  }

  static void destroyAbstractFuture(AnyReference value)
  {
    value.destroy();
  }

  void ServiceBoundObject::onMessage(const qi::Message &msg, TransportSocketPtr socket) {
    boost::mutex::scoped_lock lock(_callMutex);
    try {
      if (msg.version() > qi::Message::currentVersion())
      {
        std::stringstream ss;
        ss << "Cannot negotiate QiMessaging connection: "
           << "remote end doesn't support binary protocol v" << msg.version();
        serverResultAdapter(qi::makeFutureError<AnyReference>(ss.str()), Signature(),
                            _gethost(), socket, msg.address(), Signature(), CancelableKitWeak());
        return;
      }

      qiLogDebug() << this << "(" << service() << '/' << _objectId << ") msg " << msg.address() << " " << msg.buffer().size();

      if (msg.object() > _objectId)
      {
        qiLogDebug() << "Passing message to children";
        ObjectHost::onMessage(msg, socket);
        return;
      }

      qi::AnyObject    obj;
      unsigned int     funcId;
      //choose between special function (on BoundObject) or normal calls
      // Manageable functions are at the end of reserver range but dispatch to _object
      if (msg.function() < Manageable::startId) {
        obj = _self;
      } else {
        obj = _object;
      }
      funcId = msg.function();

      qi::Signature sigparam;
      GenericFunctionParameters mfp;

      // Validate call target
      if (msg.type() == qi::Message::Type_Call) {
        const qi::MetaMethod *mm = obj.metaObject().method(funcId);
        if (!mm) {
          std::stringstream ss;
          ss << "No such method " << msg.address();
          qiLogError() << ss.str();
          throw std::runtime_error(ss.str());
        }
        sigparam = mm->parametersSignature();
      }

      else if (msg.type() == qi::Message::Type_Post) {
        const qi::MetaSignal *ms = obj.metaObject().signal(funcId);
        if (ms)
          sigparam = ms->parametersSignature();
        else {
          const qi::MetaMethod *mm = obj.metaObject().method(funcId);
          if (mm)
            sigparam = mm->parametersSignature();
          else {
            qiLogError() << "No such signal/method on event message " << msg.address();
            return;
          }
        }
      }
      else if (msg.type() == qi::Message::Type_Cancel)
      {
        AnyReference ref = msg.value("I", socket);
        unsigned int origMsgId = ref.to<unsigned int>();
        ref.destroy();
        cancelCall(socket, msg, origMsgId);
        return;
      }
      else
      {
        qiLogError() << "Unexpected message type " << msg.type() << " on " << msg.address();
        return;
      }

      AnyReference value;
      if (msg.flags() & Message::TypeFlag_DynamicPayload)
        sigparam = "m";
      // ReturnType flag appends a signature to the payload
      Signature originalSignature;
      bool hasReturnType = (msg.flags() & Message::TypeFlag_ReturnType) != 0;
      if (hasReturnType)
      {
        originalSignature = sigparam;
        sigparam = "(" + sigparam.toString() + "s)";
      }
      value = msg.value(sigparam, socket);
      std::string returnSignature;
      if (hasReturnType)
      {
        returnSignature = value[1].to<std::string>();
        value[1].destroy();
        value = value[0];
        sigparam = originalSignature;
      }
      if (sigparam == "m")
      {
        // received dynamically typed argument pack, unwrap
        AnyValue* content = value.ptr<AnyValue>();
        // steal it
        AnyReference pContent = content->release();

        // free the object content
        value.destroy();
        value = pContent;
      }
      mfp = value.asTupleValuePtr();
      /* Because of 'global' _currentSocket, we cannot support parallel
      * executions at this point.
      * Both on self, and on obj which can use currentSocket() too.
      *
      * So put a lock, and rely on metaCall we invoke being asynchronous for// execution
      * This is decided by _callType, set from BoundObject ctor argument, passed by Server, which
      * uses its internal _defaultCallType, passed to its constructor, default
      * to queued. When Server is instanciated by ObjectHost, it uses the default
      * value.
      *
      * As a consequence, users of currentSocket() must set _callType to Direct.
      * Calling currentSocket multiple times in a row should be avoided.
      */
      switch (msg.type())
      {
      case Message::Type_Call: {
        boost::recursive_mutex::scoped_lock lock(_mutex);
        _currentSocket = socket;
        qi::MetaCallType mType = obj == _self ? MetaCallType_Direct : _callType;
        qi::Signature sig = returnSignature.empty() ? Signature() : Signature(returnSignature);
        qi::Future<AnyReference>  fut = obj.metaCall(funcId, mfp, mType, sig);
        AtomicIntPtr cancelRequested = boost::make_shared<Atomic<int> >(0);
        {
          qiLogDebug() << "Registering future for " << socket.get() << ", message:" << msg.id();
          boost::mutex::scoped_lock futlock(_cancelables->guard);
          _cancelables->map[socket][msg.id()] = std::make_pair(fut, cancelRequested);
        }
        Signature retSig;
        const MetaMethod* mm = obj.metaObject().method(funcId);
        if (mm)
          retSig = mm->returnSignature();
        _currentSocket.reset();
        fut.connect(boost::bind<void>
                    (&ServiceBoundObject::serverResultAdapter, _1, retSig, _gethost(), socket, msg.address(), sig,
                     CancelableKitWeak(_cancelables), cancelRequested));
      }
        break;
      case Message::Type_Post: {
        if (obj == _self) // we need a sync call (see comment above), post does not provide it
          obj.metaCall(funcId, mfp, MetaCallType_Direct);
        else
          obj.metaPost(funcId, mfp);
      }
        break;
      default:
        qiLogError() << "unknown request of type " << (int)msg.type() << " on service: " << msg.address();
      }
      //########################
      value.destroy();
    } catch (const std::runtime_error &e) {
      if (msg.type() == Message::Type_Call) {
        qi::Promise<AnyReference> prom;
        prom.setError(e.what());
        serverResultAdapter(prom.future(), Signature(), _gethost(), socket, msg.address(), Signature(),
                            CancelableKitWeak(_cancelables));
      }
    } catch (...) {
      if (msg.type() == Message::Type_Call) {
        qi::Promise<AnyReference> prom;
        prom.setError("Unknown error catch");
        serverResultAdapter(prom.future(), Signature(), _gethost(), socket, msg.address(), Signature(),
                            CancelableKitWeak(_cancelables));
      }
    }
  }

  void ServiceBoundObject::cancelCall(TransportSocketPtr socket, const Message& cancelMessage, MessageId origMsgId)
  {
    qiLogDebug() << "Canceling call: " << origMsgId << " on client " << socket.get();
    std::pair<Future<AnyReference>, AtomicIntPtr > fut;
    {
      boost::mutex::scoped_lock lock(_cancelables->guard);
      CancelableMap& cancelableCalls = _cancelables->map;
      CancelableMap::iterator it = cancelableCalls.find(socket);
      if (it == cancelableCalls.end())
      {
        qiLogDebug() << "Socket " << socket.get() << " not recorded";
        return;
      }
      FutureMap::iterator futIt = it->second.find(origMsgId);

      if (futIt == it->second.end())
      {
        qiLogDebug() << "No recorded future for message " << origMsgId;
        return;
      }
      fut = futIt->second;
    }

    // We count the number or requested cancels.
    // ServerResultAdapter can also process some cancels.
    // We want the total amount of effective cancels be equal to
    // how many times cancel has been requested.
    int cancelCount = ++(*fut.second);
    Future<AnyReference>& future = fut.first;
    future.cancel();

    FutureState state = future.wait(0);
    if (state == FutureState_FinishedWithValue)
    {
      _removeCachedFuture(CancelableKitWeak(_cancelables), socket, origMsgId);
      // Check if we have an underlying future: in that case it needs
      // to be cancelled as well.
      AnyReference val = future.value();
      boost::shared_ptr<GenericObject> ao = qi::detail::getGenericFuture(val);
      if (!ao)
      {
        qiLogDebug() << "Message " << origMsgId << ": return value is not a future.";
        return;
      }

      // Check if serverResultAdapter hasn't run before us and is taking care of
      // cancelling the inner future.
      bool doCancel = false;
      while (cancelCount)
      {
        if (fut.second->setIfEquals(cancelCount, cancelCount - 1))
        {
          doCancel = true;
          break;
        }
        cancelCount = **fut.second;
      }
      if (!doCancel)
      {
        return;
      }
      // This outer future is 'done', so its completion callback has already been
      // called or is in the process of being called (that would be serverResultAdapter).
      // It will register a completion callback on its inner future (if applicable),
      // so we just need to call cancel.
      ao->call<void>("cancel");
      qiLogInfo() << "Cancelled message " << origMsgId;
    }
  }

  void ServiceBoundObject::onSocketDisconnected(TransportSocketPtr client, std::string error)
  {
    // Disconnect event links set for this client.
    if (_onSocketDisconnectedCallback)
      _onSocketDisconnectedCallback(client, error);
    {
      boost::mutex::scoped_lock lock(_cancelables->guard);
      _cancelables->map.erase(client);
    }
    BySocketServiceSignalLinks::iterator it = _links.find(client);
    if (it != _links.end())
    {
      for (ServiceSignalLinks::iterator jt = it->second.begin(); jt != it->second.end(); ++jt)
      {
        try
        {
          _object.disconnect(jt->second.localSignalLinkId);
        }
        catch (const std::runtime_error& e)
        {
          qiLogError() << e.what();
        }
      }
      _links.erase(it);
    }
    removeRemoteReferences(client);
  }

  qi::BoundAnyObject makeServiceBoundAnyObject(unsigned int serviceId, qi::AnyObject object, qi::MetaCallType mct) {
    boost::shared_ptr<ServiceBoundObject> ret = boost::make_shared<ServiceBoundObject>(serviceId, Message::GenericObject_Main, object, mct);
    return ret;
  }

  AnyValue ServiceBoundObject::property(const AnyValue& prop)
  {
    if (prop.kind() == TypeKind_String)
      return _object.property<AnyValue>(prop.toString());
    else if (prop.kind() == TypeKind_Int)
    { // missing accessor, go to bacend
      GenericObject* go = _object.asGenericObject();
      return go->type->property(go->value, _object, (unsigned int)prop.toUInt());
    }
    else
      throw std::runtime_error("Expected int or string for property index");
  }

  void ServiceBoundObject::setProperty(const AnyValue& prop, AnyValue val)
  {
    qi::Future<void> result;
    if (prop.kind() == TypeKind_String)
      result = _object.setProperty(prop.toString(), val);
    else if (prop.kind() == TypeKind_Int)
    {
      GenericObject* go = _object.asGenericObject();
      result = go->type->setProperty(go->value, _object, (unsigned int)prop.toUInt(), val);
    }
    else
      throw std::runtime_error("Expected int or string for property index");
    if (!result.isFinished())
      qiLogWarning() << "Assertion failed, setProperty() call not finished";
    // Throw the future error
    result.value();
  }

  std::vector<std::string> ServiceBoundObject::properties()
  {
    // FIXME implement
    std::vector<std::string> res;
    const MetaObject& mo = _object.metaObject();
    MetaObject::PropertyMap map = mo.propertyMap();
    for (MetaObject::PropertyMap::iterator it = map.begin(); it != map.end(); ++it)
      res.push_back(it->second.name());
    return res;
  }

  void ServiceBoundObject::_removeCachedFuture(CancelableKitWeak kit, TransportSocketPtr sock, MessageId id)
  {
    CancelableKitPtr kitPtr = kit.lock();
    if (!kitPtr)
      return;

    boost::mutex::scoped_lock lock(kitPtr->guard);
    CancelableMap& cancelableCalls = kitPtr->map;
    CancelableMap::iterator it = cancelableCalls.find(sock);

    if (it != cancelableCalls.end())
    {
      FutureMap::iterator futIt = it->second.find(id);
      if (futIt != it->second.end())
      {
        it->second.erase(futIt);
        if (it->second.size() == 0)
          cancelableCalls.erase(it);
      }
    }
  }

  static inline void convertAndSetValue(Message& ret, AnyReference val,
    const Signature& targetSignature, ObjectHost* host, TransportSocket* socket,
    const Signature& forcedSignature)
  {
    // We allow forced signature conversion to fail, in which case we
    // go on with original expected signature.

    if (forcedSignature.isValid() && socket->remoteCapability("MessageFlags", false))
    {
      std::pair<AnyReference, bool> conv = val.convert(TypeInterface::fromSignature(forcedSignature));
      qiLogDebug("qimessaging.serverresult") << "Converting to forced signature " << forcedSignature.toString()
        << ", data=" << val.type()->infoString() <<", advertised=" <<targetSignature.toString() << ", success="
        << conv.second;
      if (conv.first.type())
      {
        ret.setValue(conv.first, "m", host, socket);
        ret.addFlags(Message::TypeFlag_DynamicPayload);
        if (conv.second)
          conv.first.destroy();
        return;
      }
    }
    ret.setValue(val, targetSignature, host, socket);
  }

  // second bounce when returned type is a future
  void ServiceBoundObject::serverResultAdapterNext(AnyReference val,// the future
    Signature targetSignature,ObjectHost* host, TransportSocketPtr socket, const qi::MessageAddress &replyaddr,
    const Signature& forcedReturnSignature, CancelableKitWeak kit)
  {
    qi::Message ret(Message::Type_Reply, replyaddr);
    _removeCachedFuture(kit, socket, replyaddr.messageId);
    try {
      TypeKind kind;
      boost::shared_ptr<GenericObject> ao = qi::detail::getGenericFuture(val, &kind);
      if (ao->call<bool>("hasError", 0))
      {
        ret.setType(qi::Message::Type_Error);
        ret.setError(ao->call<std::string>("error", 0));
      }
      else if (ao->call<bool>("isCanceled"))
      {
        qiLogDebug() << "Call " << replyaddr.messageId << " has been canceled.";
        if (!socket->sharedCapability("RemoteCancelableCalls", false))
        {
          ret.setType(Message::Type_Error);
          ret.setError("Call has been canceled.");
        }
        else
          ret.setType(Message::Type_Canceled);
      }
      else
      {
        // Future<void>::value() give a void* so we need a special handling to
        // produce a real void
        AnyValue value;
        if (kind == TypeKind_Void)
          value = AnyValue(qi::typeOf<void>());
        else
          value = ao->call<AnyValue>("value", 0);
        convertAndSetValue(ret, value.asReference(), targetSignature, host, socket.get(), forcedReturnSignature);
      }
    } catch (const std::exception &e) {
      //be more than safe. we always want to nack the client in case of error
      ret.setType(qi::Message::Type_Error);
      ret.setError(std::string("Uncaught error:") + e.what());
    } catch (...) {
      //be more than safe. we always want to nack the client in case of error
      ret.setType(qi::Message::Type_Error);
      ret.setError("Unknown error caught while forwarding the answer");
    }
    if (!socket->send(ret))
      qiLogWarning("qimessaging.serverresult") << "Can't generate an answer for address:" << replyaddr;
    val.destroy();
  }

  void ServiceBoundObject::serverResultAdapter(Future<AnyReference> future, const qi::Signature& targetSignature,
                                               ObjectHost* host, TransportSocketPtr socket,
                                               const qi::MessageAddress &replyaddr,
                                               const Signature& forcedReturnSignature, CancelableKitWeak kit,
                                               AtomicIntPtr cancelRequested)
  {
    qi::Message ret(Message::Type_Reply, replyaddr);
    if (future.hasError()) {
      ret.setType(qi::Message::Type_Error);
      ret.setError(future.error());
    } else if (future.isCanceled()) {
      ret.setType(Message::Type_Canceled);
      qiLogDebug() << "Call " << replyaddr.messageId << " was cancelled.";
    } else {
      try {
        qi::AnyReference val = future.value();
        boost::shared_ptr<GenericObject> ao = qi::detail::getGenericFuture(val);
        if (ao)
        {
          boost::function<void()> cb = boost::bind(&ServiceBoundObject::serverResultAdapterNext, val, targetSignature,
                                                   host, socket, replyaddr, forcedReturnSignature, kit);
          ao->call<void>("_connect", cb);
          // Check if the atomic is set to true.
          // If it is and we manage to set it to false, we're taking care of cancelling the future.
          if (cancelRequested)
          {
            int cancelCount = *(*cancelRequested);
            bool doCancel = false;
            while (cancelCount)
            {
              if (cancelRequested->setIfEquals(cancelCount, cancelCount - 1))
              {
                doCancel = true;
                break;
              }
              cancelCount = **cancelRequested;
            }
            if (doCancel)
            {
              qiLogDebug() << "Cancel requested for call " << replyaddr.messageId;
              ao->call<void>("cancel");
            }
          }
          return;
        }
        convertAndSetValue(ret, val, targetSignature, host, socket.get(), forcedReturnSignature);
        future.setOnDestroyed(&destroyAbstractFuture);
      } catch (const std::exception &e) {
        //be more than safe. we always want to nack the client in case of error
        ret.setType(qi::Message::Type_Error);
        ret.setError(std::string("Uncaught error:") + e.what());
      } catch (...) {
        //be more than safe. we always want to nack the client in case of error
        ret.setType(qi::Message::Type_Error);
        ret.setError("Unknown error caught while sending the answer");
      }
    }
    _removeCachedFuture(kit, socket, replyaddr.messageId);
    if (!socket->send(ret))
      qiLogWarning("qimessaging.serverresult") << "Can't generate an answer for address:" << replyaddr;
  }

}
