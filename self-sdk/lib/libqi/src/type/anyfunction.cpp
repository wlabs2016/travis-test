/*
**  Copyright (C) 2012 Aldebaran Robotics
**  See COPYING for the license
*/
#include <boost/noncopyable.hpp>
#include <qi/future.hpp>
#include <qi/signature.hpp>
#include <qi/anyfunction.hpp>
#include <qi/anyobject.hpp>

qiLogCategory("qitype.functiontype");

namespace qi
{

  //make destroy exception-safe for AnyFunction::call
  class AnyReferenceArrayDestroyer : private boost::noncopyable {
  public:
    AnyReferenceArrayDestroyer(AnyReference *toDestroy, void **convertedArgs, bool shouldDelete)
      : toDestroy(toDestroy)
      , convertedArgs(convertedArgs)
      , toDestroyPos(0)
      , shouldDelete(shouldDelete)
    {
    }

    ~AnyReferenceArrayDestroyer() {
      destroy();
    }

    void destroy() {
      if (toDestroy) {
        for (unsigned i = 0; i < toDestroyPos; ++i)
          toDestroy[i].destroy();
        if (shouldDelete)
          delete[] toDestroy;
        toDestroy = 0;
      }
      if (shouldDelete && convertedArgs) {
        delete[] convertedArgs;
        convertedArgs = 0;
      }
    }


  public:
    AnyReference* toDestroy;
    void**        convertedArgs;
    unsigned int  toDestroyPos;
    bool          shouldDelete;
  };



  AnyReference AnyFunction::call(AnyReference arg1, const AnyReferenceVector& remaining)
  {
    AnyReferenceVector args;
    args.reserve(remaining.size()+1);
    args.push_back(arg1);
    args.insert(args.end(), remaining.begin(), remaining.end());
    return call(args);
  }

  AnyReference AnyFunction::call(const AnyReferenceVector& vargs)
  {
    if (type == dynamicFunctionTypeInterface())
    {
      DynamicFunction* f = (DynamicFunction*)value;
      if (!transform.dropFirst && !transform.prependValue)
        return (*f)(vargs);
      AnyReferenceVector args;
      if (transform.dropFirst && !transform.prependValue)
      {
        // VCXX2008 does not accept insert here because GV(GVP) ctor is explicit
        args.resize(vargs.size()-1);
        for (unsigned i=0; i<vargs.size()-1; ++i)
          args[i] = vargs[i+1];
      }
      else if (transform.dropFirst && transform.prependValue)
      {
        args = vargs;
        args[0] = AnyReference(args[0].type(), transform.boundValue);
      }
      else // prepend && ! drop
        throw std::runtime_error("Cannot prepend argument to dynamic function type");
      return (*f)(args);
    }

    /* We must honor transform, who can have any combination of the following enabled:
    * - drop first arg
    * - prepend an arg
    */
    unsigned deltaCount = (transform.dropFirst? -1:0) + (transform.prependValue?1:0);
    const std::vector<TypeInterface*>& target = type->argumentsType();
    unsigned sz = vargs.size();
    const AnyReference* args = sz > 0 ? &vargs[0] : 0;

    if (target.size() != sz + deltaCount)
    {
      throw std::runtime_error(_QI_LOG_FORMAT("Argument count mismatch, expected %1%, got %2%",
        target.size(), sz + deltaCount));
      return AnyReference();
    }
    if (transform.dropFirst)
    {
      ++args;
      --sz;
    }
    unsigned offset = transform.prependValue? 1:0;
#if QI_HAS_VARIABLE_LENGTH_ARRAY
    AnyReference sstoDestroy[sz+offset];
    void* ssconvertedArgs[sz+offset];
    AnyReferenceArrayDestroyer arad(sstoDestroy, ssconvertedArgs, false);
#else
    AnyReference* sstoDestroy = new AnyReference[sz+offset];
    void** ssconvertedArgs = new void*[sz+offset];
    AnyReferenceArrayDestroyer arad(sstoDestroy, ssconvertedArgs, true);
#endif
    if (transform.prependValue)
      arad.convertedArgs[0] = transform.boundValue;
    for (unsigned i=0; i<sz; ++i)
    {
      const unsigned ti = i + offset;
      //qiLogDebug() << "argument " << i
      //   << " " << args[i].type->infoString() << ' ' << args[i].value
      //   << " to " << target[ti]->infoString();
      if (args[i].type() == target[ti] || args[i].type()->info() == target[ti]->info())
        arad.convertedArgs[ti] = args[i].rawValue();
      else
      {
        //qiLogDebug() << "needs conversion "
        //<< args[i].type->infoString() << " -> "
        //<< target[ti]->infoString();
        std::pair<AnyReference,bool> v = args[i].convert(target[ti]);
        if (!v.first.type())
        {
          // Try pointer dereference
          if (args[i].kind() == TypeKind_Pointer)
          {
            AnyReference deref = *const_cast<AnyReference&>(args[i]);
            if (deref.type() == target[ti] || deref.type()->info() == target[ti]->info())
              v = std::make_pair(deref, false);
            else
              v = deref.convert(target[ti]);
          }
          if (!v.first.type())
          {
            throw std::runtime_error(_QI_LOG_FORMAT("Call argument number %d conversion failure from %s to %s. Function signature: %s.",
                                                    i,
                                                    args[i].type()->signature().toPrettySignature(),
                                                    target[ti]->signature().toPrettySignature(),
                                                    this->parametersSignature(this->transform.dropFirst).toPrettySignature()));

            return AnyReference();
          }
        }

        if (v.second)
          arad.toDestroy[arad.toDestroyPos++] = v.first;
        arad.convertedArgs[ti] = v.first.rawValue();
      }
    }
    void* res;
    res = type->call(value, arad.convertedArgs, sz+offset);
    arad.destroy();
    return AnyReference(resultType(), res);
  }

  const AnyFunction& AnyFunction::dropFirstArgument() const
  {
    transform.dropFirst = true;
    return *this;
  }

  const AnyFunction& AnyFunction::prependArgument(void* arg) const
  {
    transform.prependValue = true;
    transform.boundValue = arg;
    return *this;
  }

  const AnyFunction& AnyFunction::replaceFirstArgument(void* arg) const
  {
    transform.dropFirst = true;
    return prependArgument(arg);
  }

  TypeInterface* AnyFunction::resultType() const
  {
    return type->resultType();
  }

  std::vector<TypeInterface*> AnyFunction::argumentsType() const
  {
    std::vector<TypeInterface*> res = type->argumentsType();
    if (transform.dropFirst && transform.prependValue) // optimize that case
      res[0] = typeOf<AnyValue>();
    else if (transform.dropFirst)
    {
      // First argument passed to us will be ignored, so apparent signature
      // has one extra arg of any type.

      // do not access res[1], it might not exist and invalid access might be
      // detected by debug-mode stl
      res.push_back(0);
      memmove(&res[0]+1, &res[0], (res.size()-1)*sizeof(TypeInterface*));
      res[0] = typeOf<AnyValue>();
    }
    else if (transform.prependValue)
    {
      // We bind one argument, so it is not present in apparent signature, remove it
      memmove(&res[0], &res[0]+1, (res.size()-1)*sizeof(TypeInterface*));
      res.pop_back();
    }
    return res;
  }

  qi::Signature AnyFunction::parametersSignature(bool dropFirst) const
  {
    if (type == dynamicFunctionTypeInterface())
      return "m";
    if (!dropFirst)
      return qi::makeTupleSignature(argumentsType());
    std::vector<TypeInterface*> vtype = argumentsType();
    if (vtype.empty())
      throw std::runtime_error("Can't drop the first argument, the argument list is empty");
    //ninja! : drop the first TypeInterface*
    memmove(&vtype[0], &vtype[0]+1, (vtype.size()-1)*sizeof(TypeInterface*));
    vtype.pop_back();
    return qi::makeTupleSignature(vtype);
  }

  qi::Signature AnyFunction::returnSignature() const
  {
    if (type == dynamicFunctionTypeInterface())
      return "m";
    // Detect if returned type is a qi::Future and return underlying value type.
    TypeOfTemplate<Future>* ft1 = QI_TEMPLATE_TYPE_GET(resultType(), Future);
    TypeOfTemplate<FutureSync>* ft2 = QI_TEMPLATE_TYPE_GET(resultType(), FutureSync);
    if (ft1)
      return ft1->templateArgument()->signature();
    else if (ft2)
      return ft2->templateArgument()->signature();
    else
      return resultType()->signature();
  }

  qi::Signature CallableTypeInterface::parametersSignature() const
  {
    if (this == dynamicFunctionTypeInterface())
      return "m";
    else
      return qi::makeTupleSignature(_argumentsType);
  }

  qi::Signature CallableTypeInterface::returnSignature() const
  {
    if (this == dynamicFunctionTypeInterface())
      return "m";
    return _resultType->signature();
  }

  GenericFunctionParameters::GenericFunctionParameters()
  {
  }

  GenericFunctionParameters::GenericFunctionParameters(const AnyReferenceVector& args)
  :AnyReferenceVector(args)
  {
  }

  GenericFunctionParameters GenericFunctionParameters::copy(bool notFirst) const
  {
    GenericFunctionParameters result(*this);
    for (unsigned i=notFirst?1:0; i<size(); ++i)
      result[i] = result[i].clone();
    return result;
  }

  void GenericFunctionParameters::destroy(bool notFirst)
  {
    for (unsigned i = notFirst ? 1 : 0; i < size(); ++i)
      (*this)[i].destroy();
  }

  GenericFunctionParameters
  GenericFunctionParameters::convert(const Signature& sig) const
  {
    GenericFunctionParameters dst;
    const AnyReferenceVector& src = *this;
    if (sig.children().size() != src.size())
    {
      qiLogError() << "convert: signature/params size mismatch"
      << sig.toString() << " " << sig.children().size() << " " << src.size();
      return dst;
    }
    const SignatureVector &elts = sig.children();
    SignatureVector::const_iterator it = elts.begin();
    int idx = 0;
    for (;it != elts.end(); ++it,++idx)
    {
      TypeInterface* compatible = qi::TypeInterface::fromSignature(*it);
      if (!compatible)
      {
        qiLogError() << "convert: unknown type " << (*it).toString();
        compatible = src[idx].type();
      }
      dst.push_back(src[idx].convertCopy(compatible));
    }
    return dst;
  }

  Signature GenericFunctionParameters::signature(bool dyn) const
  {
    const AnyReferenceVector& params = *this;
    return qi::makeTupleSignature(params, dyn);
  }

  class DynamicFunctionTypeInterfaceInterface: public FunctionTypeInterface
  {
  public:
    DynamicFunctionTypeInterfaceInterface()
    {
      _resultType = typeOf<AnyValue>();
    }
    virtual void* call(void* func, void** args, unsigned int argc)
    {
      qiLogError() << "Dynamic function called without type information";
      return 0;
    }
    _QI_BOUNCE_TYPE_METHODS(DefaultTypeImplMethods<DynamicFunction>);
  };

  FunctionTypeInterface* dynamicFunctionTypeInterface()
  {
    static FunctionTypeInterface* type = 0;
    if (!type)
      type = new DynamicFunctionTypeInterfaceInterface();
    return type;
  }

  AnyFunction AnyFunction::fromDynamicFunction(DynamicFunction f)
  {
    FunctionTypeInterface* d = dynamicFunctionTypeInterface();
    AnyFunction result(d, d->clone(d->initializeStorage(&f)));
    return result;
  }

}
#ifdef QITYPE_TRACK_FUNCTIONTYPE_INSTANCES
#include <qi/application.hpp>
namespace qi
{
  namespace detail
  {
    static std::map<std::string, int> functionTrackMap;
    void functionTypeTrack(const std::string& f)
    {
      functionTrackMap[std::string(f)]++;
    }
    void functionTypeDump()
    {
      for (std::map<std::string, int>::iterator it = functionTrackMap.begin();
        it != functionTrackMap.end(); ++it)
        std::cerr << it->second << '\t' << it->first << std::endl;
    }
  }
}
#endif
