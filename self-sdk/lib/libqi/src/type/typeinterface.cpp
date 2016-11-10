/*
**  Copyright (C) 2012 Aldebaran Robotics
**  See COPYING for the license
*/

#include <boost/lexical_cast.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/algorithm/string.hpp>

#include <qi/type/typeinterface.hpp>
#include <qi/signature.hpp>
#include <qi/anyvalue.hpp>
#include <qi/anyobject.hpp>
#include <qi/type/typedispatcher.hpp>
#include <qi/anyfunction.hpp>

#ifdef __GNUC__
#include <cxxabi.h>
#endif

qiLogCategory("qitype.type");

namespace qi {


  TypeInfo::TypeInfo()
  : stdInfo(0)
  {}

  TypeInfo::TypeInfo(const std::type_info& info)
  : stdInfo(&info)
  {
  }

  TypeInfo::TypeInfo(const std::string& str)
  : stdInfo(0), customInfo(str)
  {

  }

  std::string TypeInfo::asString() const
  {
    if (stdInfo)
      return stdInfo->name();
    else
      return customInfo;
  }

  std::string TypeInfo::asDemangledString() const
  {
#ifdef __GNUC__
    if (stdInfo) {
      std::string tmp;
      int status;
      char *demangled = abi::__cxa_demangle(stdInfo->name(), NULL, NULL, &status);
      if (status == 0) {
        tmp = demangled;
        free(demangled);
        return tmp;
      } else {
        return stdInfo->name();
      }
    } else {
      return customInfo;
    }
#else
    return asString();
#endif
  }

  const char* TypeInfo::asCString() const
  {
    if (stdInfo)
      return stdInfo->name();
    else
      return customInfo.c_str();
  }

  bool TypeInfo::operator==(const TypeInfo& b) const
  {
    if (!! stdInfo != !! b.stdInfo)
      return false;
    if (stdInfo)
#ifdef __APPLE__
      // on osx, non-exported typeinfos do not compare equal between libraries
      return strcmp(stdInfo->name(), b.stdInfo->name()) == 0;
#else
      return *stdInfo == *b.stdInfo;
#endif
    else
      return customInfo == b.customInfo;
  }

  bool TypeInfo::operator!=(const TypeInfo& b) const
  {
    return ! (*this == b);
  }

  bool TypeInfo::operator< (const TypeInfo& b) const
  {
    if (!! stdInfo != !! b.stdInfo)
      return stdInfo != 0;
    else
    {
      if (stdInfo)
#ifdef __APPLE__
        // on osx, non-exported typeinfos do not compare equal between libraries
        return strcmp(stdInfo->name(), b.stdInfo->name()) < 0;
#else
        return (*stdInfo).before(*b.stdInfo) != 0;
#endif
      else
        return customInfo < b.customInfo;
    }
  }

  typedef std::map<TypeInfo, TypeInterface*> TypeFactory;
  static TypeFactory& typeFactory()
  {
    static TypeFactory* res = 0;
    QI_THREADSAFE_NEW(res);
    return *res;
  }

  typedef std::map<std::string, TypeInterface*> FallbackTypeFactory;
  static FallbackTypeFactory& fallbackTypeFactory()
  {
    static FallbackTypeFactory* res = 0;
    QI_THREADSAFE_NEW(res);
    return *res;
  }

  QI_API TypeInterface* getType(const std::type_info& type)
  {
    static boost::mutex* mutex = 0;
    QI_THREADSAFE_NEW(mutex);
    boost::mutex::scoped_lock sl(*mutex);
    static bool fallback = !qi::os::getenv("QI_TYPE_RTTI_FALLBACK").empty();

    // We create-if-not-exist on purpose: to detect access that occur before
    // registration
    TypeInterface* result = typeFactory()[TypeInfo(type)];
    if (result || !fallback)
      return result;
    result = fallbackTypeFactory()[type.name()];
    if (result)
      qiLogError("qitype.type") << "RTTI failure for " << type.name();
    return result;
  }

  /// Type factory setter
  QI_API bool registerType(const std::type_info& typeId, TypeInterface* type)
  {
    qiLogCategory("qitype.type"); // method can be called at static init
    qiLogDebug() << "registerType "  << typeId.name() << " "
     << type->kind() <<" " << (void*)type << " " << type->signature().toString();
    TypeFactory::iterator i = typeFactory().find(TypeInfo(typeId));
    if (i != typeFactory().end())
    {
      if (i->second)
        qiLogVerbose() << "registerType: previous registration present for "
          << typeId.name()<< " " << (void*)i->second << " " << i->second->kind();
      else
        qiLogVerbose() << "registerType: access to type factory before"
          " registration detected for type " << typeId.name();
    }
    typeFactory()[TypeInfo(typeId)] = type;
    fallbackTypeFactory()[typeId.name()] = type;
    return true;
  }

  class SignatureTypeVisitor
  {
  public:
    SignatureTypeVisitor(AnyReference value, bool resolveDynamic)
    : _value(value)
    , _resolveDynamic(resolveDynamic)
    {
    }

    void visitVoid()
    {
      result = qi::Signature::fromType(Signature::Type_Void);
    }

    void visitInt(int64_t, bool isSigned, int byteSize)
    {
      switch((isSigned?1:-1)*byteSize)
      {
        case 0:  result = qi::Signature::fromType(Signature::Type_Bool);  break;
        case 1:  result = qi::Signature::fromType(Signature::Type_Int8);  break;
        case -1: result = qi::Signature::fromType(Signature::Type_UInt8); break;
        case 2:  result = qi::Signature::fromType(Signature::Type_Int16); break;
        case -2: result = qi::Signature::fromType(Signature::Type_UInt16);break;
        case 4:  result = qi::Signature::fromType(Signature::Type_Int32); break;
        case -4: result = qi::Signature::fromType(Signature::Type_UInt32);break;
        case 8:  result = qi::Signature::fromType(Signature::Type_Int64); break;
        case -8: result = qi::Signature::fromType(Signature::Type_UInt64);break;
        default:
          result =  qi::Signature::fromType(Signature::Type_Unknown);
      }
    }

    void visitFloat(double, int byteSize)
    {
      if (byteSize == 4)
        result = qi::Signature::fromType(Signature::Type_Float);
      else
        result = qi::Signature::fromType(Signature::Type_Double);
    }

    void visitString(char*, size_t)
    {
      result = qi::Signature::fromType(Signature::Type_String);
    }

    void visitList(AnyIterator it, AnyIterator iend)
    {
      qi::Signature esig = static_cast<ListTypeInterface*>(_value.type())->elementType()->signature();
      if (!_resolveDynamic || it == iend) {
        result = qi::makeListSignature(esig);
        return;
      }

      qi::Signature sigFirst = (*it).signature(true);
      ++it;
      for (;it != iend && sigFirst.isValid(); ++it)
      {
        qi::Signature sig = (*it).signature(true);
        if (sig != sigFirst)
        {
          if (sig.isConvertibleTo(sigFirst))
          {} // keep sigFirst
          else if (sigFirst.isConvertibleTo(sig))
            sigFirst = sig; // keep sig
          else
          {
            qiLogDebug() << "Heterogeneous elements "
                         << sigFirst.toString() << ' ' << sig.toString();
            sigFirst = qi::Signature();
          }
        }
      }
      // qiLogDebug() << "List effective " << sigFirst;
      result = qi::makeListSignature(sigFirst.isValid()?sigFirst:esig);
    }

    void visitVarArgs(AnyIterator it, AnyIterator iend)
    {
      qi::Signature esig = static_cast<ListTypeInterface*>(_value.type())->elementType()->signature();
      result = qi::makeVarArgsSignature(esig);
    }

    void visitMap(AnyIterator it, AnyIterator iend)
    {
      MapTypeInterface* type =  static_cast<MapTypeInterface*>(_value.type());
      if (!_resolveDynamic || it == iend) {
        result = qi::makeMapSignature(type->keyType()->signature(), type->elementType()->signature());
        return;
      }

      AnyReference e = *it;
      qi::Signature ksig = e[0].signature(true);
      qi::Signature vsig = e[1].signature(true);
      // Check that ksig/vsig is always the same, set to empty if not
      ++it;
      for(; it!= iend; ++it)
      {
        AnyReference e = *it;
        qi::Signature k = e[0].signature(true);
        qi::Signature v = e[1].signature(true);
        if (ksig.isValid() && ksig != k)
        {
          if (k.isConvertibleTo(ksig))
          {}
          else if (ksig.isConvertibleTo(k))
            ksig = k;
          else
          {
            qiLogDebug() << "Heterogeneous keys " << ksig.toString() << e[0].signature(true).toString();
            ksig = qi::Signature();
          }
        }
        if (vsig.isValid() && vsig != v)
        {
          if (v.isConvertibleTo(vsig))
          {}
          else if (vsig.isConvertibleTo(v))
            vsig = v;
          else
          {
            qiLogDebug() << "Heterogeneous value " << vsig.toString() << e[1].signature(true).toString();
            vsig = qi::Signature();
          }
        }
      }
      // qiLogDebug() << "Map effective: " << ksig << " , " << vsig;
      result = qi::makeMapSignature((ksig.isValid()?ksig:type->keyType()->signature()),
                                    (vsig.isValid()?vsig:type->elementType()->signature()));
    }


    void visitObject(GenericObject )
    {
      result = qi::Signature::fromType(Signature::Type_Object);
    }

    void visitAnyObject(AnyObject& )
    {
      result = qi::Signature::fromType(Signature::Type_Object);
    }

    void visitPointer(AnyReference)
    {
      result = qi::Signature::fromType(Signature::Type_Unknown);
    }

    void visitUnknown(AnyReference)
    {
      result = qi::Signature::fromType(Signature::Type_Unknown);
    }

    void visitTuple(const std::string &name, const AnyReferenceVector& vals, const std::vector<std::string>& annotations)
    {
      std::string res;
      res = qi::makeTupleSignature(vals, _resolveDynamic).toString();

      if (annotations.size() >= vals.size()) {

        res += '<';
        if (!name.empty())
          res += name;

        for (unsigned i = 0; i < vals.size(); ++i)
        {
          res += ',' + annotations[i];
        }
        res += '>';
      }
      result = qi::Signature(res);
    }

    void visitDynamic(AnyReference pointee)
    {
      if (_resolveDynamic)
        result = pointee.signature(true);
      else
        result = qi::Signature::fromType(Signature::Type_Dynamic);
    }

    void visitRaw(AnyReference)
    {
      result = qi::Signature::fromType(Signature::Type_Raw);
    }

    void visitIterator(AnyReference v)
    {
      visitUnknown(v);
    }

    qi::Signature  result;
    AnyReference  _value;
    bool          _resolveDynamic;
  };

  Signature TypeInterface::signature(void* storage, bool resolveDynamic)
  {
    if (resolveDynamic)
    {
      AnyReference value(this, storage);
      SignatureTypeVisitor ts(value, resolveDynamic);
      typeDispatch(ts, value);
      return qi::Signature(ts.result);
    }
    else
    { // We might be called without a valid storage in that mode, which
      // is not supported by typeDispatch(), so we copy pasted a safer version
      // of typeDispatch()
      // Still reuse methods from SignatureTypeVisitor to avoid duplication
      AnyReference value(this, storage);
      SignatureTypeVisitor v(value, resolveDynamic);
      switch(kind())
      {
      case TypeKind_Void:
        return qi::Signature::fromType(Signature::Type_Void);
      case TypeKind_Int:
      {
        IntTypeInterface* tint = static_cast<IntTypeInterface*>(value.type());
        v.visitInt(0, tint->isSigned(), tint->size());
        break;
      }
      case TypeKind_Float:
      {
        FloatTypeInterface* tfloat = static_cast<FloatTypeInterface*>(value.type());
        v.visitFloat(0, tfloat->size());
        break;
      }
      case TypeKind_String:
        v.result = qi::Signature::fromType(Signature::Type_String);
        break;
      case TypeKind_List:
        v.visitList(AnyIterator(), AnyIterator());
        break;
      case TypeKind_Map:
        v.visitMap(AnyIterator(), AnyIterator());
        break;
      case TypeKind_Object:
        v.result = qi::Signature::fromType(Signature::Type_Object);
        break;
      case TypeKind_Pointer:
      {
        PointerTypeInterface* type = static_cast<PointerTypeInterface*>(value.type());
        TypeKind pointedKind = type->pointedType()->kind();
        if (type->pointerKind() == PointerTypeInterface::Shared
          && (pointedKind == TypeKind_Object || pointedKind == TypeKind_Unknown))
        {
          if(pointedKind != TypeKind_Object)
            qiLogVerbose() << "Shared pointer to unknown type " << type->pointedType()->infoString()
                           << ", assuming object not yet registered";
          AnyObject op;
          v.visitAnyObject(op);
        }
        else
        {
          qiLogVerbose() << "Pointer to unknown type " << type->pointedType()->infoString() << ", signature is X";
          v.visitPointer(AnyReference());
        }
        break;
      }
      case TypeKind_Tuple: {
        std::vector<TypeInterface*>       memberTypes = static_cast<StructTypeInterface*>(this)->memberTypes();
        std::vector<std::string> annotations = static_cast<StructTypeInterface*>(this)->elementsName();
        std::string              name        = static_cast<StructTypeInterface*>(this)->className();
        v.result = qi::makeTupleSignature(memberTypes, name, annotations);
        break;
      }
      case TypeKind_Dynamic:
        if (value.type()->info() == typeOf<AnyObject>()->info())
          v.result = qi::Signature::fromType(Signature::Type_Object);
        else
          v.result = qi::Signature::fromType(Signature::Type_Dynamic);
        break;
      case TypeKind_Raw:
        v.result = qi::Signature::fromType(Signature::Type_Raw);
        break;
      case TypeKind_Unknown:
        v.result = qi::Signature::fromType(Signature::Type_Unknown);
        break;
      case TypeKind_VarArgs: {
        TypeInterface* elt = static_cast<VarArgsTypeInterface*>(this)->elementType();
        v.result = qi::makeVarArgsSignature(elt->signature());
        break;
      }
      case TypeKind_Iterator:
      case TypeKind_Function:
      case TypeKind_Signal:
      case TypeKind_Property:
        throw std::runtime_error("Cannot get signature of iterator, function, signal or property");
      }

      return v.result;
    }
  }

  TypeInterface* makeFloatType(int bytelen)
  {
    static TypeInterface* tfloat = typeOf<float>();
    static TypeInterface* tdouble = typeOf<double>();
    if (bytelen == 4)
      return tfloat;
    else if (bytelen == 8)
      return tdouble;
    throw std::runtime_error("Invalid bytelen");
  }

  TypeInterface* makeIntType(bool issigned, int bytelen)
  {
    static TypeInterface* tb;
    static TypeInterface* t8;
    static TypeInterface* t16;
    static TypeInterface* t32;
    static TypeInterface* t64;
    static TypeInterface* tu8;
    static TypeInterface* tu16;
    static TypeInterface* tu32;
    static TypeInterface* tu64;
    QI_ONCE(
      tb = typeOf<bool>();
      t8 = typeOf<int8_t>();
      t16 = typeOf<int16_t>();
      t32 = typeOf<int32_t>();
      t64 = typeOf<int64_t>();
      tu8  = typeOf<uint8_t>();
      tu16 = typeOf<uint16_t>();
      tu32 = typeOf<uint32_t>();
      tu64 = typeOf<uint64_t>();
      );

    if (issigned) {
      switch (bytelen) {
        case 0:
          return tb;
        case 1:
          return t8;
        case 2:
          return t16;
        case 4:
          return t32;
        case 8:
          return t64;
      }
    } else {
      switch (bytelen) {
        case 0:
          return tb;
        case 1:
          return tu8;
        case 2:
          return tu16;
        case 4:
          return tu32;
        case 8:
          return tu64;
      }
    }
    throw std::runtime_error("Invalid bytelen");
  }


  TypeInterface* makeTypeOfKind(const qi::TypeKind& kind)
  {
    static TypeInterface* tv;
    static TypeInterface* t64;
    static TypeInterface* tdouble;
    static TypeInterface* tstring;
    static TypeInterface* tgv;
    static TypeInterface* tbuffer;
    static TypeInterface* tobjectptr;
    QI_ONCE(
      tv = typeOf<void>();
      t64 = typeOf<int64_t>();
      tdouble = typeOf<double>();
      tstring = typeOf<std::string>();
      tgv = typeOf<AnyValue>();
      tbuffer = typeOf<Buffer>();
      tobjectptr = typeOf<AnyObject>();
      );

    switch(kind)
    {
    case TypeKind_Void:
      return tv;
    case TypeKind_Int:
      return t64;
    case TypeKind_Float:
      return tdouble;
    case TypeKind_String:
      return tstring;
    case TypeKind_Dynamic:
      return tgv;
    case TypeKind_Raw:
      return tbuffer;
    case TypeKind_Object:
      return tobjectptr;
    default:
      qiLogWarning() << "Cannot get type from kind " << kind;
      return 0;
    }
  }

  static TypeInterface* fromSignature(const qi::Signature& sig)
  {
    static TypeInterface* tv;
    static TypeInterface* tb;
    static TypeInterface* t8;
    static TypeInterface* t16;
    static TypeInterface* t32;
    static TypeInterface* t64;
    static TypeInterface* tu8 ;
    static TypeInterface* tu16;
    static TypeInterface* tu32;
    static TypeInterface* tu64;
    static TypeInterface* tfloat;
    static TypeInterface* tdouble;
    static TypeInterface* tstring;
    static TypeInterface* tgv;
    static TypeInterface* tbuffer;
    static TypeInterface* tobjectptr;
    QI_ONCE(
      tv = typeOf<void>();
      tb = typeOf<bool>();
      t8 = typeOf<int8_t>();
      t16 = typeOf<int16_t>();
      t32 = typeOf<int32_t>();
      t64 = typeOf<int64_t>();
      tu8  = typeOf<uint8_t>();
      tu16 = typeOf<uint16_t>();
      tu32 = typeOf<uint32_t>();
      tu64 = typeOf<uint64_t>();
      tfloat = typeOf<float>();
      tdouble = typeOf<double>();
      tstring = typeOf<std::string>();
      tgv = typeOf<AnyValue>();
      tbuffer = typeOf<Buffer>();
      tobjectptr = typeOf<AnyObject>();
      )
    switch(sig.type())
    {
    case Signature::Type_None:
    case Signature::Type_Void:
      return tv;
    case Signature::Type_Bool:
      return tb;
    case Signature::Type_Int8:
      return t8;
    case Signature::Type_UInt8:
      return tu8;
    case Signature::Type_Int16:
      return t16;
    case Signature::Type_UInt16:
      return tu16;
    case Signature::Type_Int32:
      return t32;
    case Signature::Type_UInt32:
      return tu32;
    case Signature::Type_Int64:
      return t64;
    case Signature::Type_UInt64:
      return tu64;
    case Signature::Type_Float:
      return tfloat;
    case Signature::Type_Double:
      return tdouble;
    case Signature::Type_String:
      return tstring;
    case Signature::Type_List:
      {
        TypeInterface* el = fromSignature(sig.children().at(0));
        if (!el)
        {
          qiLogError() << "Cannot get type from list of unknown type.";
          return 0;
        }
      return makeListType(el);
      }
    case Signature::Type_VarArgs:
      {
        TypeInterface* el = fromSignature(sig.children().at(0));
        if (!el)
        {
          qiLogError() << "Cannot get type from varargs of unknown type.";
          return 0;
        }
      return makeVarArgsType(el);
      }
    case Signature::Type_Map:
      {
        TypeInterface* k = fromSignature(sig.children().at(0));
        TypeInterface* e = fromSignature(sig.children().at(1));
        if (!k || !e)
        {
          qiLogError() <<" Cannot get type from map of unknown "
          << (k?"element":"key") << " type";
          return 0;
        }
        return makeMapType(k, e);
      }
    case Signature::Type_Tuple:
      {
        // Look it up in dynamically generated oportunistic factory.
        TypeInterface* res = getRegisteredStruct(sig);
        if (res)
          return res;
        // Failure, synthetise a type.
        std::vector<TypeInterface*> types;
        const SignatureVector& c = sig.children();
        for (SignatureVector::const_iterator child = c.begin(); child != c.end(); child++)
        {
          TypeInterface* t = fromSignature(*child);
          if (!t)
          {
            qiLogError() << "Cannot get type from tuple of unknown element type " << child->toString();
            return 0;
          }
          // qiLogDebug() << "tuple element " << child.signature() << " " << t->infoString();
          types.push_back(t);
        }
        std::vector<std::string> vannotations;
        std::string annotation = sig.annotation();
        boost::algorithm::split(vannotations, annotation, boost::algorithm::is_any_of(","));
        //first annotation is the name, then the name of each elements
        if (vannotations.size() >= 1)
          res = makeTupleType(types, vannotations[0], std::vector<std::string>(vannotations.begin()+1, vannotations.end()));
        else
          res = makeTupleType(types);
        //qiLogDebug() <<"Resulting tuple " << sig.toString() << " " << res->infoString();
        return res;
      }
    case Signature::Type_Dynamic:
      return tgv;
    case Signature::Type_Raw:
      return tbuffer;
    case Signature::Type_Object:
      return tobjectptr;
    default:
      qiLogWarning() << "Cannot get type from signature " << sig.toString();
      return 0;
    }
  }


  TypeInterface* TypeInterface::fromSignature(const qi::Signature& sig)
  {
    TypeInterface* result = ::qi::fromSignature(sig);
    // qiLogDebug() << "fromSignature() " << i.signature() << " -> " << (result?result->infoString():"NULL");
    return result;
  }

  // Default list
  static TypeInterface* makeListIteratorType(TypeInterface* element);

  class DefaultListIteratorType: public TypeSimpleIteratorImpl<std::vector<void*>::iterator >
  {
  public:
  private:
    DefaultListIteratorType(TypeInterface* elementType)
    : _elementType(elementType)
    {
      // We need an unique name, but elementType->nifo().aString() is not
      // guaranteed unique. So use our address. The factory system ensures
      // non-duplication.
      _name = "DefaultListIteratorType<"
        + _elementType->info().asString()
        + ">(" + boost::lexical_cast<std::string>(this);
      _info = TypeInfo(_name);
    }
    friend TypeInterface* makeListIteratorType(TypeInterface*);
  public:
    AnyReference dereference(void* storage)
    {
      std::vector<void*>::iterator& ptr = *(std::vector<void*>::iterator*)ptrFromStorage(&storage);
      return AnyReference(_elementType, *ptr);
    }
    const TypeInfo& info()
    {
      return _info;
    }
    TypeInterface* _elementType;
    std::string _name;
    TypeInfo _info;
  };

  // We want exactly one instance per element type
  static TypeInterface* makeListIteratorType(TypeInterface* element)
  {
    static boost::mutex* mutex;
    QI_THREADSAFE_NEW(mutex);
    boost::mutex::scoped_lock lock(*mutex);
    static std::map<TypeInfo, TypeInterface*>* map = 0;
    if (!map)
      map = new std::map<TypeInfo, TypeInterface*>();
    TypeInfo key = element->info();
    std::map<TypeInfo, TypeInterface*>::iterator it;
    TypeInterface* result;
    it = map->find(key);
    if (it == map->end())
    {
      result = new DefaultListIteratorType(element);
      (*map)[key] = result;
    }
    else
      result = it->second;
    return result;
  }

  template <typename T>
  class DefaultListTypeBase: public T
  {
  protected:
    DefaultListTypeBase(const std::string& name, TypeInterface* elementType)
    : _elementType(elementType)
    {
       _name = name + "<"
        + _elementType->info().asString()
        + ">(" + boost::lexical_cast<std::string>(this);
        _info = TypeInfo(_name);
    }
  public:
    TypeInterface* elementType()
    {
      return _elementType;
    }
    AnyIterator begin(void* storage)
    {
      std::vector<void*>& ptr = *(std::vector<void*>*)ptrFromStorage(&storage);
      std::vector<void*>::iterator it = ptr.begin();
      AnyReference v = AnyReference::from(it);
      // Hugly type swap, works because we know backend storage matches
      v = AnyReference(makeListIteratorType(_elementType), v.rawValue());
      return AnyIterator(v);
    }
    AnyIterator end(void* storage)
    {
      std::vector<void*>& ptr = *(std::vector<void*>*)ptrFromStorage(&storage);
      std::vector<void*>::iterator it = ptr.end();
      AnyReference v = AnyReference::from(it);
      // Hugly type swap, works because we know backend storage matches
      v = AnyReference(makeListIteratorType(_elementType), v.rawValue());
      return AnyIterator(v);
    }

    void* clone(void* storage)
    {
      std::vector<void*>& src = *(std::vector<void*>*)ptrFromStorage(&storage);
      void* result = initializeStorage();
      std::vector<void*>& dst = *(std::vector<void*>*)ptrFromStorage(&result);
      for (unsigned i=0; i<src.size(); ++i)
        dst.push_back(_elementType->clone(src[i]));
      return result;
    }

    void destroy(void* storage)
    {
      std::vector<void*>& src = *(std::vector<void*>*)ptrFromStorage(&storage);
      for (unsigned i=0; i<src.size(); ++i)
        _elementType->destroy(src[i]);
      Methods::destroy(storage);
    }

    void pushBack(void** storage, void* valueStorage)
    {
      std::vector<void*>& src = *(std::vector<void*>*)ptrFromStorage(storage);
      src.push_back(_elementType->clone(valueStorage));
    }

    void* element(void* storage, int key)
    {
      std::vector<void*>& src = *(std::vector<void*>*)ptrFromStorage(&storage);
      return src[key];
    }
    const TypeInfo& info()
    {
      return _info;
    }
    typedef DefaultTypeImplMethods<std::vector<void*>, TypeByPointerPOD<std::vector<void*> > > Methods;
    void* initializeStorage(void* ptr=0) { return Methods::initializeStorage(ptr); }
    void* ptrFromStorage(void**s)        { return Methods::ptrFromStorage(s); }

    TypeInterface* _elementType;
    std::string    _name;
    TypeInfo       _info;
  };

  class DefaultVarArgsType: public DefaultListTypeBase<VarArgsTypeInterfaceImpl< qi::VarArguments<void*> > >
  {
  private:
    DefaultVarArgsType(TypeInterface* elementType)
      : DefaultListTypeBase<VarArgsTypeInterfaceImpl< qi::VarArguments<void*> > >("DefaultVarArgsType", elementType)
    {}
  public:
    friend TypeInterface* makeVarArgsType(TypeInterface* element);
  };

  class DefaultListType: public DefaultListTypeBase<ListTypeInterfaceImpl< std::vector<void*> > >
  {
  private:
    DefaultListType(TypeInterface* elementType)
      : DefaultListTypeBase<ListTypeInterfaceImpl< std::vector<void*> > >("DefaultListType", elementType)
    {}
  public:
    friend TypeInterface* makeListType(TypeInterface* element);
  };

  TypeInterface* makeVarArgsType(TypeInterface* element)
  {
    static boost::mutex* mutex = 0;
    QI_THREADSAFE_NEW(mutex);
    boost::mutex::scoped_lock lock(*mutex);
    static std::map<TypeInfo, TypeInterface*>* map = 0;
    if (!map)
      map = new std::map<TypeInfo, TypeInterface*>();
    TypeInfo key(element->info());
    std::map<TypeInfo, TypeInterface*>::iterator it;
    TypeInterface* result;
    it = map->find(key);
    if (it == map->end())
    {
      result = new DefaultVarArgsType(element);
      (*map)[key] = result;
    }
    else
      result = it->second;
    return result;
  }
    // We want exactly one instance per element type
  TypeInterface* makeListType(TypeInterface* element)
  {
    static boost::mutex* mutex = 0;
    QI_THREADSAFE_NEW(mutex);
    boost::mutex::scoped_lock lock(*mutex);
    static std::map<TypeInfo, TypeInterface*>* map = 0;
    if (!map)
      map = new std::map<TypeInfo, TypeInterface*>();
    TypeInfo key(element->info());
    std::map<TypeInfo, TypeInterface*>::iterator it;
    TypeInterface* result;
    it = map->find(key);
    if (it == map->end())
    {
      result = new DefaultListType(element);
      (*map)[key] = result;
    }
    else
      result = it->second;
    return result;
  }



  class DefaultTupleType: public StructTypeInterface
  {
  private:
    DefaultTupleType(const std::vector<TypeInterface*>& types, const std::string& className = std::string(), const std::vector<std::string>& elementsName = std::vector<std::string>())
    : _className(className)
    , _types(types)
    , _elementName(elementsName)
    {
      _name = "DefaultTupleType<";
      for (unsigned i=0; i<types.size(); ++i)
        _name += types[i]->info().asString() + ",";
      _name += ">(" + boost::lexical_cast<std::string>(this) + ")";
      qiLogDebug() << "Instanciating tuple " << _name;
      _info = TypeInfo(_name);
    }

    friend TypeInterface* makeTupleType(const std::vector<TypeInterface*>&, const std::string&, const std::vector<std::string>&);

  public:
    virtual std::vector<TypeInterface*> memberTypes() { return _types;}

    virtual void* get(void* storage, unsigned int index)
    {
      std::vector<void*>& ptr = *(std::vector<void*>*)ptrFromStorage(&storage);
      if (ptr.size() < index +1)
        ptr.resize(index + 1, 0);
      return ptr[index];
    }

    virtual void set(void** storage, unsigned int index, void* valStorage)
    {
      std::vector<void*>& ptr = *(std::vector<void*>*)ptrFromStorage(storage);
      if (ptr.size() < index +1)
        ptr.resize(index + 1, 0);
      if (ptr[index])
        _types[index]->destroy(ptr[index]);
      ptr[index] = _types[index]->clone(valStorage);
    }

    const TypeInfo& info()
    {
      return _info;
    }

    virtual void* clone(void* storage)
    {
      std::vector<void*>& src = *(std::vector<void*>*)ptrFromStorage(&storage);
      void* result = initializeStorage();
      for (unsigned i=0; i<src.size(); ++i)
        set(&result, i, src[i]); // set will clone
      return result;
    }

    virtual void destroy(void* storage)
    { // destroy elements that have been set
      std::vector<void*>& ptr = *(std::vector<void*>*)ptrFromStorage(&storage);
      for (unsigned i=0; i<ptr.size(); ++i)
      {
        _types[i]->destroy(ptr[i]);
      }
      Methods::destroy(storage);
    }

    void* initializeStorage(void* ptr=0) {
      std::vector<void*> *ret = (std::vector<void*>*)Methods::initializeStorage(ptr);
      if (ptr)
      {
        if (_types.size() != ret->size())
          throw std::runtime_error("Tuple storage is of incorrect size");
      }
      else
      {
        ret->resize(_types.size());
        for (unsigned i=0; i < ret->size(); ++i) {
          (*ret)[i] = _types[i]->initializeStorage();
        }
      }
      return ret;
    }

    virtual void* ptrFromStorage(void**s) { return Methods::ptrFromStorage(s);}

    std::vector<void*>& backend(void* storage)
    {
      std::vector<void*>& ptr = *(std::vector<void*>*)ptrFromStorage(&storage);
      return ptr;
    }

    virtual std::vector<std::string> elementsName() {
      return _elementName;
    }

    virtual std::string className() {
      return _className;
    }

    bool less(void* a, void* b) { return Methods::less(a, b);}

  public:
    std::string              _className;
    std::vector<TypeInterface*>       _types;
    std::vector<std::string> _elementName;
    std::string              _name;
    TypeInfo                 _info;
    typedef DefaultTypeImplMethods<std::vector<void*>, TypeByPointerPOD<std::vector<void*> > > Methods;
  };

  AnyReference makeGenericTuple(const AnyReferenceVector& values)
  {
    std::vector<TypeInterface*> types;
    types.reserve(values.size());
    for (unsigned i=0; i<values.size(); ++i) {
      types.push_back(values[i].type());
    }
    AnyReference result(makeTupleType(types));
    result.setTuple(values);
    return result;
  }

  AnyReference makeGenericTuplePtr(
    const std::vector<TypeInterface*>&types,
    const std::vector<void*>&values)
  {
    StructTypeInterface* tupleType = static_cast<StructTypeInterface*>(makeTupleType(types));
    return AnyReference(tupleType, tupleType->initializeStorage((void*)(const void*)&values));
  }




  // element of map is of type _pairType, see below
  typedef std::map<AnyReference, void*> DefaultMapStorage;

  // Default map, using a vector<pair<void*, void*> > as storage
  static TypeInterface* makeMapIteratorType(TypeInterface* kt);

  class DefaultMapIteratorType: public IteratorTypeInterface
  {
  public:
  private:
    DefaultMapIteratorType(TypeInterface* elementType)
    : _elementType(elementType)
    {
      _name = "DefaultMapIteratorType<"
      + elementType->info().asString()
      + "(" + boost::lexical_cast<std::string>(this) + ")";
      _info = TypeInfo(_name);
    }
    friend TypeInterface* makeMapIteratorType(TypeInterface* kt);
  public:
    AnyReference dereference(void* storage)
    {
      /* Result is a pair<GV, void*>
       * and we must return something we store, pretending it is of
       * type pair<K&, V&>
       * The *pair itself* must be somehow stored
       *
       *
      */
      DefaultMapStorage::iterator& it = *(DefaultMapStorage::iterator*)
        ptrFromStorage(&storage);
      return AnyReference(_elementType, it->second);
    }
    void next(void** storage)
    {
      DefaultMapStorage::iterator& ptr = *(DefaultMapStorage::iterator*)
        ptrFromStorage(storage);
      ++ptr;
    }
    bool equals(void* s1, void* s2)
    {
      DefaultMapStorage::iterator& p1 = *(DefaultMapStorage::iterator*)
        ptrFromStorage(&s1);
      DefaultMapStorage::iterator& p2 = *(DefaultMapStorage::iterator*)
        ptrFromStorage(&s2);
      return p1 == p2;
    }
    const TypeInfo& info()
    {
      return _info;
    }
    typedef DefaultTypeImplMethods<DefaultMapStorage::iterator, TypeByPointerPOD<DefaultMapStorage::iterator> > Impl;
    _QI_BOUNCE_TYPE_METHODS_NOINFO(Impl);
    TypeInterface* _elementType;
    std::string _name;
    TypeInfo _info;
  };

  // We want exactly one instance per element type
  static TypeInterface* makeMapIteratorType(TypeInterface* te)
  {
    typedef std::map<TypeInfo, TypeInterface*> Map;
    static boost::mutex* mutex = 0;
    QI_THREADSAFE_NEW(mutex);
    boost::mutex::scoped_lock lock(*mutex);

    static Map * map = 0;
    if (!map)
      map = new Map();
    TypeInfo ti(te->info());
    Map::key_type key(ti);
    Map::iterator it;
    TypeInterface* result;
    it = map->find(key);
    if (it == map->end())
    {
      result = new DefaultMapIteratorType(te);
      (*map)[key] = result;
    }
    else
      result = it->second;
    return result;
  }

  class DefaultMapType: public MapTypeInterface
  {
  public:
  private:
    DefaultMapType(TypeInterface* keyType, TypeInterface* elementType)
    : _keyType(keyType)
    , _elementType(elementType)
    {
      _name = "DefaultMapType<"
      + keyType->info().asString() + ", "
      + elementType->info().asString()
      + "(" + boost::lexical_cast<std::string>(this) + ")";
      _info = TypeInfo(_name);
      std::vector<TypeInterface*> kvtype;
      kvtype.push_back(_keyType);
      kvtype.push_back(_elementType);
      _pairType = static_cast<DefaultTupleType*>(makeTupleType(kvtype));
      assert(dynamic_cast<DefaultTupleType*>(_pairType));
    }
    friend TypeInterface* makeMapType(TypeInterface* kt, TypeInterface* et);
  public:
    TypeInterface* elementType()
    {
      return _elementType;
    }
    TypeInterface* keyType ()
    {
      return _keyType;
    }
    AnyIterator begin(void* storage)
    {
      DefaultMapStorage& ptr = *(DefaultMapStorage*)ptrFromStorage(&storage);
      DefaultMapStorage::iterator it = ptr.begin();
      AnyReference val = AnyReference::from(it);
      val = AnyReference(makeMapIteratorType(_pairType), val.rawValue());
      return AnyIterator(val);
    }
    AnyIterator end(void* storage)
    {
      DefaultMapStorage& ptr = *(DefaultMapStorage*)ptrFromStorage(&storage);
      DefaultMapStorage::iterator it = ptr.end();
      AnyReference val = AnyReference::from(it);
      val = AnyReference(makeMapIteratorType(_pairType), val.rawValue());
      return AnyIterator(val);
    }

    // Unconditional insert, assumes key is not present, return value
    AnyReference _insert(DefaultMapStorage& ptr, void* keyStorage, void* valueStorage, bool copyValue)
    {
      // key is referenced in map key, and map value for the pair
      AnyReference key(_keyType, keyStorage);
      key = key.clone();
      AnyReference value(_elementType, valueStorage);
      if (copyValue)
        value = value.clone();
      // must cast or wrong AnyReference ctor is called
      // We know that _pairType is a DefaultTupleType, so optimize:
      // if we construct a value from _pairType it will allocate the pair content
      void* pairPtr = DefaultTupleType::Methods::initializeStorage();
      std::vector<void*>&pair = *(std::vector<void*>*) pairPtr;
      pair.resize(2);
      pair[0] = key.rawValue();
      pair[1] = value.rawValue();
      ptr[key] = pairPtr;
      return value;
    }

    void insert(void** storage, void* keyStorage, void* valueStorage)
    {
      DefaultMapStorage& ptr = *(DefaultMapStorage*)ptrFromStorage(storage);
      DefaultMapStorage::iterator i = ptr.find(AnyReference(_keyType, keyStorage));
      if (i != ptr.end())
      {// Replace: clear previous storage
        // Now, normally tuple (_pairType is one) only have inplace set
        // But this is not any tuple, we know it's a DefaultTuple
        // So we need to hack.
        std::vector<void*>& elem = _pairType->backend(i->second);
        assert(elem.size() == 2);
        _elementType->destroy(elem[1]);
        elem[1] = AnyReference(_elementType, valueStorage).clone().rawValue();
      }
      else
      {
        _insert(ptr, keyStorage, valueStorage, true);
      }
    }

    AnyReference element(void** pstorage, void* keyStorage, bool autoInsert)
    {
      DefaultMapStorage& ptr = *(DefaultMapStorage*) ptrFromStorage(pstorage);
      DefaultMapStorage::iterator i = ptr.find(AnyReference(_keyType, keyStorage));
      if (i != ptr.end())
      {
        AnyReference elem(_pairType, i->second);
        return elem[1];
      }
      if (!autoInsert)
        return AnyReference();
      return _insert(ptr, keyStorage, _elementType->initializeStorage(), false);
    }

    size_t size(void* storage)
    {
      DefaultMapStorage& ptr = *(DefaultMapStorage*) ptrFromStorage(&storage);
      return ptr.size();
    }
    void destroy(void* storage)
    {
      DefaultMapStorage& ptr = *(DefaultMapStorage*)ptrFromStorage(&storage);
      for (DefaultMapStorage::iterator it = ptr.begin(); it != ptr.end(); ++it)
      {
        // destroying the pair will destroy key and value
        _pairType->destroy(it->second);
      }
      Methods::destroy(storage);
    }
    void* clone(void* storage)
    {
      void* result = initializeStorage();
      DefaultMapStorage& src = *(DefaultMapStorage*)ptrFromStorage(&storage);
      DefaultMapStorage& dst = *(DefaultMapStorage*)ptrFromStorage(&result);
      // must clone content
      for (DefaultMapStorage::iterator it = src.begin(); it != src.end(); ++it)
      {
        // do not double-clone the key, which is in the pair also
        AnyReference clonedPair(_pairType, _pairType->clone(it->second));
        dst[clonedPair[0]] = clonedPair.rawValue();
      }
      return result;
    }
    const TypeInfo& info()
    {
      return _info;
    }
    typedef DefaultTypeImplMethods<DefaultMapStorage, TypeByPointerPOD<DefaultMapStorage> > Methods;
    void* initializeStorage(void* ptr=0) { return Methods::initializeStorage(ptr);}   \
    virtual void* ptrFromStorage(void**s) { return Methods::ptrFromStorage(s);}
    bool less(void* a, void* b) { return Methods::less(a, b);}
    TypeInterface* _keyType;
    TypeInterface* _elementType;
    DefaultTupleType* _pairType;
    TypeInfo _info;
    std::string _name;
  };


  // We want exactly one instance per element type
  TypeInterface* makeMapType(TypeInterface* kt, TypeInterface* et)
  {
    static boost::mutex* mutex = 0;
    QI_THREADSAFE_NEW(mutex);
    boost::mutex::scoped_lock lock(*mutex);

    typedef std::map<std::pair<TypeInfo, TypeInfo>, MapTypeInterface*> Map;
    static Map * map = 0;
    if (!map)
      map = new Map();
    TypeInfo kk(kt->info());
    TypeInfo ek(et->info());
    Map::key_type key(kk, ek);
    Map::iterator it;
    MapTypeInterface* result;
    it = map->find(key);
    if (it == map->end())
    {
      result = new DefaultMapType(kt, et);
      (*map)[key] = result;
    }
    else
    {
      result = it->second;
    }
    return result;
  }

  struct InfosKey
  {
  public:
    InfosKey(const std::vector<TypeInterface*>& types, const std::string &name = std::string(), const std::vector<std::string>& elements = std::vector<std::string>())
      : _types(types)
      , _name(name)
      , _elements(elements)
    {}

    bool operator < (const InfosKey& b) const
    {
      //check for types
      if (_types.size() != b._types.size())
        return _types.size() < b._types.size();
      for (unsigned i = 0; i < _types.size(); ++i)
      {
        if ( _types[i]->info() != b._types[i]->info())
          return _types[i]->info() < b._types[i]->info();
      }

      //check for name
      if (_name != b._name)
        return _name < b._name;

      //check for elements
      if (_elements.size() != b._elements.size())
        return _elements.size() < b._elements.size();
      for (unsigned i = 0; i < _elements.size(); ++i)
      {
        if ( _elements[i] != b._elements[i])
          return _elements[i] < b._elements[i];
      }
      return false;
    }

  private:
    std::vector<TypeInterface*>       _types;
    std::string              _name;
    std::vector<std::string> _elements;
  };

  //TODO: not threadsafe
  TypeInterface* makeTupleType(const std::vector<TypeInterface*>& types, const std::string &name, const std::vector<std::string>& elementNames)
  {
    typedef std::map<InfosKey, StructTypeInterface*> Map;
    static boost::mutex* mutex;
    QI_THREADSAFE_NEW(mutex);
    boost::mutex::scoped_lock lock(*mutex);
    static Map* map = 0;
    if (!map)
      map = new Map;
    InfosKey key(types, name, elementNames);
    Map::iterator it = map->find(key);
    if (it == map->end())
    {
      StructTypeInterface* result = new DefaultTupleType(types, name, elementNames);
      (*map)[key] = result;
      return result;
    }
    else
    {
      StructTypeInterface* res = it->second;
      assert(res->memberTypes().size() == types.size());
      return res;
    }
  }

  void* ListTypeInterface::element(void* storage, int index)
  {
    // Default implementation using iteration
    AnyReference self(this, storage);
    AnyIterator it = self.begin();
    AnyIterator iend = self.end();
    int p = 0;
    while (p!= index && it!= iend)
    {
      ++p;
      ++it;
    }
    if (p > index)
      throw std::runtime_error("Index out of range");
    return (*it).rawValue();
  }

  namespace detail
  {
    void typeFail(const char* typeName, const char* operation)
    {
      /* Use an internal map and be untemplated to avoid generating zillions
      * of symbols
      */
      std::ostringstream ss;
      ss << "Cannot do '" << operation << "' on " << typeName;
      static std::set<std::string>* once = 0;
      QI_THREADSAFE_NEW(once);
      if (once->find(typeName)==once->end())
      {
        once->insert(typeName);
        qiLogError() << ss.str();
      }
      throw std::runtime_error(ss.str());
    }

    bool fillMissingFieldsWithDefaultValues(StructTypeInterface* type,
      std::map<std::string, ::qi::AnyValue>& fields,
      const std::vector<std::string>& missing,
      const char** which, int whichLength)
    {
      // check we will get them all
      if (which)
      {
        for (unsigned i=0; i<missing.size(); ++i)
          if (std::find(which, which + whichLength, missing[i]) == which + whichLength)
            return false; // field not in handled list
      }
      std::vector<TypeInterface*> memberTypes = type->memberTypes();
      std::vector<std::string> memberNames = type->elementsName();
      for (unsigned i=0; i<missing.size(); ++i)
      { // we are being given the name, but type is known by index
        int idx = std::find(memberNames.begin(), memberNames.end(), missing[i]) - memberNames.begin();
        fields[missing[i]] = qi::AnyValue(memberTypes[idx]);
      }
      return true;
    }
  }
  static boost::mutex& registerStructMutex()
  {
    static boost::mutex* m = 0;
    QI_THREADSAFE_NEW(m);
    return *m;
  }
  typedef std::map<std::string, TypeInterface*> RegisterStructMap;
  static RegisterStructMap& registerStructMap()
  {
    // protected by lock above
    static RegisterStructMap* res = 0;
    QI_THREADSAFE_NEW(res);
    return *res;
  }
  void registerStruct(TypeInterface* type)
  {
    // leave this outside the lock!
    std::string k = type->signature().toString();
    qiLogDebug() << "Registering struct for " << k <<" " << type->infoString();
    boost::mutex::scoped_lock lock(registerStructMutex());
    registerStructMap()[k] = type;
  }
  /// @Return matchin TypeInterface registered by registerStruct() or 0.
  TypeInterface* getRegisteredStruct(const qi::Signature& s)
  {
    boost::mutex::scoped_lock lock(registerStructMutex());
    RegisterStructMap& map = registerStructMap();
    RegisterStructMap::iterator it = map.find(s.toString());
    if (it == map.end())
      return 0;
    qiLogDebug() << "Found registered struct for " << s.toString() << ": " << it->second->infoString();
      return it->second;
  }

}
