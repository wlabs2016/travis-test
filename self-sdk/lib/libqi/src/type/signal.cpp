/*
**  Copyright (C) 2012 Aldebaran Robotics
**  See COPYING for the license
*/
#include <map>
#include <qi/atomic.hpp>

#include <boost/thread/recursive_mutex.hpp>
#include <boost/make_shared.hpp>

#include <qi/signal.hpp>
#include <qi/anyvalue.hpp>
#include <qi/anyobject.hpp>

#include "signal_p.hpp"

qiLogCategory("qitype.signal");

namespace qi {

  SignalSubscriber::SignalSubscriber(const AnyObject& target, unsigned int method)
  : threadingModel(MetaCallType_Direct)
  , target(new AnyWeakObject(target))
  , method(method)
  , enabled(true)
  , executionContext(0)
  { // The slot has its own threading model: be synchronous
  }

  SignalSubscriber::SignalSubscriber(AnyFunction func, MetaCallType model)
     : handler(func), threadingModel(model), target(0), method(0), enabled(true), executionContext(0)
  {
  }

  SignalSubscriber::SignalSubscriber(AnyFunction func, ExecutionContext* ec)
     : handler(func), threadingModel(MetaCallType_Direct), target(0), method(0), enabled(true), executionContext(ec)
  {
  }

  SignalSubscriber::~SignalSubscriber()
  {
    delete target;
  }

  SignalSubscriber::SignalSubscriber(const SignalSubscriber& b)
  : enable_shared_from_this()
  , target(0)
  {
    *this = b;
  }

  void SignalSubscriber::operator=(const SignalSubscriber& b)
  {
    source = b.source;
    linkId = b.linkId;
    handler = b.handler;
    threadingModel = b.threadingModel;
    target = b.target?new AnyWeakObject(*b.target):0;
    method = b.method;
    enabled = b.enabled;
    executionContext = b.executionContext;
  }

  static qi::Atomic<int> linkUid = 1;

  void SignalBase::setCallType(MetaCallType callType)
  {
    if (!_p)
    {
      _p = boost::make_shared<SignalBasePrivate>();
    }
    _p->defaultCallType = callType;
  }

  void SignalBase::operator()(
      qi::AutoAnyReference p1,
      qi::AutoAnyReference p2,
      qi::AutoAnyReference p3,
      qi::AutoAnyReference p4,
      qi::AutoAnyReference p5,
      qi::AutoAnyReference p6,
      qi::AutoAnyReference p7,
      qi::AutoAnyReference p8)
  {
    qi::AutoAnyReference* vals[8]= {&p1, &p2, &p3, &p4, &p5, &p6, &p7, &p8};
    std::vector<qi::AnyReference> params;
    for (unsigned i = 0; i < 8; ++i)
      if (vals[i]->isValid())
        params.push_back(*vals[i]);
    qi::Signature signature = qi::makeTupleSignature(params);

    if (signature != _p->signature)
    {
      qiLogError() << "Dropping emit: signature mismatch: " << signature.toString() <<" " << _p->signature.toString();
      return;
    }
    trigger(params, _p->defaultCallType);
  }

  void SignalBase::trigger(const GenericFunctionParameters& params, MetaCallType callType)
  {
    if (!_p)
      return;
    if (_p->triggerOverride)
      _p->triggerOverride(params, callType);
    else
      callSubscribers(params, callType);
  }

  void SignalBase::setTriggerOverride(Trigger t)
  {
    if (!_p)
      _p = boost::make_shared<SignalBasePrivate>();
    _p->triggerOverride = t;
  }

  void SignalBase::setOnSubscribers(OnSubscribers onSubscribers)
  {
    if (!_p)
      _p = boost::make_shared<SignalBasePrivate>();
    _p->onSubscribers = onSubscribers;
  }

  void SignalBase::callSubscribers(const GenericFunctionParameters& params, MetaCallType callType)
  {
    MetaCallType mct = callType;

    if (!_p)
      return;

    if (mct == qi::MetaCallType_Auto)
      mct = _p->defaultCallType;
    SignalSubscriberMap copy;
    {
      boost::recursive_mutex::scoped_lock sl(_p->mutex);
      copy = _p->subscriberMap;
    }
    qiLogDebug() << (void*)this << " Invoking signal subscribers: " << copy.size();
    SignalSubscriberMap::iterator i;
    for (i = copy.begin(); i != copy.end(); ++i)
    {
      qiLogDebug() << (void*)this << " Invoking signal subscriber";
      SignalSubscriberPtr s = i->second; // hold s alive
      s->call(params, mct);
    }
    qiLogDebug() << (void*)this << " done invoking signal subscribers";
  }

  class FunctorCall
  {
  public:
    FunctorCall(GenericFunctionParameters* params, SignalSubscriberPtr* sub)
    : params(params)
    , sub(sub)
    {
    }

    FunctorCall(const FunctorCall& b)
    {
      *this = b;
    }

    void operator=(const FunctorCall& b)
    {
      params = b.params;
      sub = b.sub;
    }

    void operator() ()
    {
      try
      {
        {
          SignalSubscriberPtr s;
          boost::mutex::scoped_lock sl((*sub)->mutex);
          // verify-enabled-then-register-active op must be locked
          if (!(*sub)->enabled)
          {
            s = *sub; // delay destruction until after we leave the scoped_lock
            delete sub;
            params->destroy();
            delete params;
            return;
          }
          (*sub)->addActive(false);
        } // end mutex-protected scope
        (*sub)->handler(*params);
      }
      catch(const qi::PointerLockException&)
      {
        qiLogDebug() << "PointerLockFailure excepton, will disconnect";
      }
      catch(const std::exception& e)
      {
        qiLogWarning() << "Exception caught from signal subscriber: " << e.what();
      }
      catch (...) {
        qiLogWarning() << "Unknown exception caught from signal subscriber";
      }

      (*sub)->removeActive(true);
      params->destroy();
      delete params;
      delete sub;
    }

  public:
    GenericFunctionParameters* params;
    SignalSubscriberPtr*         sub;
  };

  void SignalSubscriber::call(const GenericFunctionParameters& args, MetaCallType callType)
  {
    // this is held alive by caller
    if (handler)
    {
      bool async = true;
      if (threadingModel != MetaCallType_Auto)
        async = (threadingModel == MetaCallType_Queued);
      else if (callType != MetaCallType_Auto)
        async = (callType == MetaCallType_Queued);

      qiLogDebug() << "subscriber call async=" << async <<" ct " << callType <<" tm " << threadingModel;
      if (executionContext || async)
      {
        GenericFunctionParameters* copy = new GenericFunctionParameters(args.copy());
        // We will check enabled when we will be scheduled in the target
        // thread, and we hold this SignalSubscriber alive, so no need to
        // explicitly track the asynccall

        // courtesy-check of el, but it should be kept alive longuer than us
        qi::ExecutionContext* ec = executionContext;
        if (!ec)
        {
          ec = getEventLoop();
          if (!ec) // this is an assert basicaly, no sense trying to do something clever.
            throw std::runtime_error("Event loop was destroyed");
        }
        ec->post(FunctorCall(copy, new SignalSubscriberPtr(shared_from_this())));
      }
      else
      {
        // verify-enabled-then-register-active op must be locked
        {
          boost::mutex::scoped_lock sl(mutex);
          if (!enabled)
            return;
          addActive(false);
        }
        //do not throw
        bool mustDisconnect = false;
        try
        {
          handler(args);
        }
        catch(const qi::PointerLockException&)
        {
          qiLogDebug() << "PointerLockFailure excepton, will disconnect";
          mustDisconnect = true;
        }
        catch(const std::exception& e)
        {
          qiLogWarning() << "Exception caught from signal subscriber: " << e.what();
        }
        catch (...)
        {
          qiLogWarning() << "Unknown exception caught from signal subscriber";
        }
        removeActive(true);
        if (mustDisconnect)
          source->disconnect(linkId);
      }
    }
    else if (target)
    {
      AnyObject lockedTarget = target->lock();
      if (!lockedTarget)
      {
        source->disconnect(linkId);
      }
      else // no need to keep anything locked, whatever happens this is not used
        lockedTarget.metaPost(method, args);
    }
  }

  //check if we are called from the same thread that triggered us.
  //in that case, do not wait.
  void SignalSubscriber::waitForInactive()
  {
    boost::thread::id tid = boost::this_thread::get_id();
    while (true)
    {
      boost::mutex::scoped_lock sl(mutex);
      if (activeThreads.empty())
        return;
      // There cannot be two activeThreads entry for the same tid
      // because activeThreads is not set at the post() stage
      if (activeThreads.size() == 1
        && *activeThreads.begin() == tid)
      { // One active callback in this thread, means above us in call stack
        // So we cannot wait for it
        return;
      }
      inactiveThread.wait(sl);
    }
  }

  void SignalSubscriber::addActive(bool acquireLock, boost::thread::id id)
  {
    if (acquireLock)
    {
      boost::mutex::scoped_lock sl(mutex);
      activeThreads.push_back(id);
    }
    else
      activeThreads.push_back(id);
  }

  void SignalSubscriber::removeActive(bool acquireLock, boost::thread::id id)
  {

    boost::mutex::scoped_lock sl(mutex, boost::defer_lock_t());
    if (acquireLock)
      sl.lock();

    for (unsigned i=0; i<activeThreads.size(); ++i)
    {
      if (activeThreads[i] == id)
      { // fast remove by swapping with last and then pop_back
        activeThreads[i] = activeThreads[activeThreads.size() - 1];
        activeThreads.pop_back();
      }
    }
    inactiveThread.notify_all();
  }

  SignalSubscriber& SignalBase::connect(AnyObject o, unsigned int slot)
  {
    return connect(SignalSubscriber(o, slot));
  }

  Signature SignalSubscriber::signature() const
  {
    if (handler)
    {
      if (handler.functionType() == dynamicFunctionTypeInterface())
        return Signature(); // no arity checking is possible
      else
        return handler.parametersSignature();
    }
    else if (target)
    {
      AnyObject locked = target->lock();
      if (!locked)
        return Signature();
      const MetaMethod* ms = locked.metaObject().method(method);
      if (!ms)
      {
        qiLogWarning() << "Method " << method <<" not found.";
        return Signature();
      }
      else
        return ms->parametersSignature();
    }
    else
      return Signature();
  }

  SignalSubscriber& SignalBase::connect(const SignalSubscriber& src)
  {
    qiLogDebug() << (void*)this << " connecting new subscriber";
    if (!_p)
    {
      _p = boost::make_shared<SignalBasePrivate>();
    }
    // Check arity. Does not require to acquire weakLock.
    int sigArity = signature().children().size();
    int subArity = -1;
    Signature subSignature = src.signature();
    if (subSignature.isValid())
      subArity = subSignature.children().size();

    if (signature() != "m" && subSignature.isValid())
    {
      if (sigArity != subArity)
      {
        std::stringstream s;
        s << "Subscriber has incorrect arity (expected "
          << sigArity << " , got " << subArity << ")";
        throw std::runtime_error(s.str());
      }
      if (!signature().isConvertibleTo(subSignature))
      {
        std::stringstream s;
        s << "Subscriber is not compatible to signal : "
          << signature().toString() << " vs " << subSignature.toString();
        throw std::runtime_error(s.str());
      }
    }

    boost::recursive_mutex::scoped_lock sl(_p->mutex);
    SignalLink res = ++linkUid;
    SignalSubscriberPtr s = boost::make_shared<SignalSubscriber>(src);
    s->linkId = res;
    s->source = this;
    bool first = _p->subscriberMap.empty();
    _p->subscriberMap[res] = s;
    if (first && _p->onSubscribers)
      _p->onSubscribers(true);
    return *s.get();
  }

  void SignalBase::createNewTrackLink(int& id, SignalLink*& pLink)
  {
    id = ++_p->trackId;
    {
      boost::recursive_mutex::scoped_lock l(_p->mutex);
      pLink = &_p->trackMap[id];
    }
  }

  void SignalBase::disconnectTrackLink(int id)
  {
    boost::recursive_mutex::scoped_lock sl(_p->mutex);
    TrackMap::iterator it = _p->trackMap.find(id);
    if (it == _p->trackMap.end())
      return;

    _p->subscriberMap.erase(it->second);
    _p->trackMap.erase(it);
  }

  bool SignalBase::disconnectAll()
  {
    if (_p)
      return _p->disconnectAll(true);
    return false;
  }

  bool SignalBase::asyncDisconnectAll()
  {
    if (_p)
      return _p->disconnectAll(false);
    return false;
  }

  SignalBase::SignalBase(const qi::Signature& sig, OnSubscribers onSubscribers)
    : _p(new SignalBasePrivate)
  {
    //Dynamic mean AnyArguments here.
    if (sig.type() != qi::Signature::Type_Dynamic && sig.type() != qi::Signature::Type_Tuple)
      throw std::runtime_error("Signal signature should be tuple, or AnyArguments");
    _p->onSubscribers = onSubscribers;
    _p->signature = sig;
  }

  SignalBase::SignalBase(OnSubscribers onSubscribers)
  : _p(new SignalBasePrivate)
  {
    _p->onSubscribers = onSubscribers;
  }


  qi::Signature SignalBase::signature() const
  {
    return _p ? _p->signature : qi::Signature();
  }

  void SignalBase::_setSignature(const qi::Signature& s)
  {
    _p->signature = s;
  }

  bool SignalBasePrivate::disconnect(const SignalLink& l, bool wait)
  {
    SignalSubscriberPtr s;
    {
      // Acquire signal mutex
      boost::recursive_mutex::scoped_lock sigLock(mutex);
      SignalSubscriberMap::iterator it = subscriberMap.find(l);
      if (it == subscriberMap.end())
        return false;
      s = it->second;
      // Remove from map (but SignalSubscriber object still good)
      subscriberMap.erase(it);
      // Acquire subscriber mutex before releasing mutex
      boost::mutex::scoped_lock subLock(s->mutex);
      // Release signal mutex
      sigLock.release()->unlock();
      // Ensure no call on subscriber occurrs once this function returns
      s->enabled = false;
      if (subscriberMap.empty() && onSubscribers)
        onSubscribers(false);
      if ( s->activeThreads.empty()
           || (s->activeThreads.size() == 1
               && *s->activeThreads.begin() == boost::this_thread::get_id()))
      { // One active callback in this thread, means above us in call stack
        // So we cannot trash s right now
        return true;
      }
      // More than one active callback, or one in a state that prevent us
      // from knowing in which thread it will run
      subLock.release()->unlock();
    }
    if (wait)
      s->waitForInactive();
    return true;
  }

  bool SignalBase::disconnect(const SignalLink &link) {
    if (!_p)
      return false;
    else
      return _p->disconnect(link, true);
  }

  bool SignalBase::asyncDisconnect(const SignalLink &link) {
    if (!_p)
      return false;
    else
      return _p->disconnect(link, false);
  }

  SignalBase::~SignalBase()
  {
  }

  SignalBasePrivate::~SignalBasePrivate()
  {
    onSubscribers = SignalBase::OnSubscribers();
    disconnectAll();
  }

  std::vector<SignalSubscriber> SignalBase::subscribers()
  {
    std::vector<SignalSubscriber> res;
    if (!_p)
      return res;
    boost::recursive_mutex::scoped_lock sl(_p->mutex);
    SignalSubscriberMap::iterator i;
    for (i = _p->subscriberMap.begin(); i!= _p->subscriberMap.end(); ++i)
      res.push_back(*i->second);
    return res;
  }

  bool SignalBase::hasSubscribers()
  {
    if (!_p)
      return false;
    boost::recursive_mutex::scoped_lock sl(_p->mutex);
    return !_p->subscriberMap.empty();
  }

  bool SignalBasePrivate::disconnectAll(bool wait)
  {
    bool ret = true;
    SignalLink link;
    while (true)
    {
      {
        boost::recursive_mutex::scoped_lock sl(mutex);
        SignalSubscriberMap::iterator it = subscriberMap.begin();
        if (it == subscriberMap.end())
          break;
        link = it->first;
      }
      // allow for multiple disconnects to occur at the same time, we must not
      // keep the lock
      bool b = disconnect(link, wait);
      if (!b)
        ret = false;
    }
    return ret;
  }

  SignalSubscriber& SignalBase::connect(AnyObject obj, const std::string& slot)
  {
    const MetaObject& mo = obj.metaObject();
    const MetaSignal* sig = mo.signal(slot);
    if (sig)
      return connect(SignalSubscriber(obj, sig->uid()));
    std::vector<MetaMethod> method = mo.findMethod(slot);
    if (method.empty())
      throw std::runtime_error("No match found for slot " + slot);
    if (method.size() > 1)
      throw std::runtime_error("Ambiguous slot name " + slot);
    return connect(SignalSubscriber(obj, method.front().uid()));
  }

  QI_API const SignalLink SignalBase::invalidSignalLink = ((unsigned int)-1);

}
