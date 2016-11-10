/*
** Copyright (C) 2014 Aldebaran
*/
#include <qi/application.hpp>
#include <qi/future.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/foreach.hpp>
#include <qi/os.hpp>
#include <qi/strand.hpp>
#include <qi/periodictask.hpp>
#include <qi/actor.hpp>
#include <qi/log.hpp>
#include <qi/anyobject.hpp>
#include <qi/type/dynamicobjectbuilder.hpp>
#include <gtest/gtest.h>

qiLogCategory("test");

void setValueWait(boost::mutex& mutex, int waittime, int& i, int v)
{
  boost::unique_lock<boost::mutex> lock(mutex, boost::try_to_lock);
  // we should never be called in parallel
  ASSERT_TRUE(lock.owns_lock());
  if (waittime)
    qi::os::msleep(waittime);
  i = v;
}

TEST(TestStrand, StrandSimple)
{
  boost::mutex mutex;

  qi::Strand strand(*qi::getEventLoop());
  int i = 0;
  qi::Future<void> f1 = strand.async(boost::bind<void>(&setValueWait,
        boost::ref(mutex), 100, boost::ref(i), 1));
  qi::os::msleep(5);
  EXPECT_FALSE(f1.isFinished());
  qi::Future<void> f2 = strand.async(boost::bind<void>(&setValueWait,
        boost::ref(mutex), 0, boost::ref(i), 2));
  qi::os::msleep(200);
  EXPECT_EQ(i, 2);
}

static void fail()
{
  ASSERT_TRUE(false);
}

TEST(TestStrand, StrandCancel)
{
  qi::Strand strand(*qi::getEventLoop());
  // cancel before scheduling
  qi::Future<void> f1 = strand.async(fail, qi::MilliSeconds(100));
  f1.cancel();
  ASSERT_EQ(qi::FutureState_Canceled, f1.wait());
}

TEST(TestStrand, StrandCancelScheduled)
{
  qi::Strand strand(*qi::getEventLoop());
  // cancel before scheduling
  qi::Future<void> f1 = strand.async(boost::bind(qi::os::msleep, 100));
  qi::Future<void> f2 = strand.async(fail);
  qi::os::msleep(30);
  f2.cancel();
  ASSERT_EQ(qi::FutureState_FinishedWithValue, f1.wait());
  ASSERT_EQ(qi::FutureState_Canceled, f2.wait());
}

static void increment(boost::mutex& mutex, int waittime, boost::atomic<unsigned int>& i)
{
  boost::unique_lock<boost::mutex> lock(mutex, boost::try_to_lock);
  // we should never be called in parallel
  ASSERT_TRUE(lock.owns_lock());
  if (waittime)
    qi::os::msleep(waittime);
  ++i;
}

static const unsigned int STRAND_NB_TRIES = 100;

TEST(TestStrand, AggressiveCancel)
{
  boost::mutex mutex;
  std::vector<qi::Future<void> > futures;

  qi::Strand strand(*qi::getEventLoop());
  boost::atomic<unsigned int> i(0);
  for (unsigned int j = 0; j < STRAND_NB_TRIES; ++j)
  {
    qi::Future<void> f1 = strand.async(boost::bind<void>(&increment,
          boost::ref(mutex), 1, boost::ref(i)));
    futures.push_back(f1);
    f1 = strand.async(boost::bind<void>(&increment,
          boost::ref(mutex), 50, boost::ref(i)));
    futures.push_back(f1);
  }
  BOOST_FOREACH(qi::Future<void>& future, futures)
    future.cancel();

  unsigned int successCount = 0;
  BOOST_FOREACH(qi::Future<void>& future, futures)
  {
    if (future.wait() != qi::FutureState_Canceled)
      successCount++;
  }
  if (i == futures.size() || i == 0)
    qiLogWarning() << "i = " << i <<
      ", this test is meaningless, you should do something about that";
  ASSERT_EQ(successCount, i);
}

TEST(TestStrand, StrandDestruction)
{
  boost::mutex mutex;
  boost::atomic<unsigned int> i(0);

  {
    qi::Strand strand(*qi::getEventLoop());
    for (unsigned int j = 0; j < STRAND_NB_TRIES; ++j)
    {
      qi::Future<void> f1 = strand.async(boost::bind<void>(&increment,
            boost::ref(mutex), 1, boost::ref(i)));
    }
  }

  ASSERT_EQ(STRAND_NB_TRIES, i);
}

TEST(TestStrand, StrandDestructionWithCancel)
{
  boost::mutex mutex;
  std::vector<qi::Future<void> > futures;
  boost::atomic<unsigned int> i(0);

  {
    qi::Strand strand(*qi::getEventLoop());
    for (unsigned int j = 0; j < STRAND_NB_TRIES; ++j)
    {
      qi::Future<void> f1 = strand.async(boost::bind<void>(&increment,
            boost::ref(mutex), 1, boost::ref(i)));
      futures.push_back(f1);
      f1 = strand.async(boost::bind<void>(&increment,
            boost::ref(mutex), 50, boost::ref(i)));
      futures.push_back(f1);
    }
    BOOST_FOREACH(qi::Future<void>& future, futures)
      future.cancel();
  }

  unsigned int successCount = 0;
  BOOST_FOREACH(qi::Future<void>& future, futures)
  {
    if (future.wait() != qi::FutureState_Canceled)
      successCount++;
  }
  if (i == futures.size() || i == 0)
    qiLogWarning() << "i = " << i <<
      ", this test is meaningless, you should do something about that";
  ASSERT_EQ(successCount, i);
}

static void deleteStrand(qi::Strand* strand)
{
  delete strand;
}

TEST(TestStrand, StrandDestructionBeforeEnd)
{
  qi::Strand* strand = new qi::Strand(*qi::getEventLoop());
  qi::Future<void> f = strand->async(boost::bind(deleteStrand, strand));
  f.value();
}

boost::atomic<int> callcount;

struct MyActor : qi::Actor
{
  boost::atomic<bool> calling;
  MyActor() : calling(0) {}
  int f(int end, qi::Promise<void> finished)
  {
    int startval = prop.get();
    EXPECT_FALSE(calling);
    calling = true;
    qi::os::msleep(5);
    EXPECT_TRUE(calling);
    calling = false;
    EXPECT_EQ(startval, prop.get());
    if (++callcount == end + 1)
      finished.setValue(0);
    return 42;
  }
  qi::Signal<int> sig;
  qi::Property<int> prop;
};
QI_REGISTER_OBJECT(MyActor, f, sig, prop);

TEST(TestStrand, AllFutureSignalPropertyPeriodicTaskAsyncTypeErasedDynamic)
{
  static const int TOTAL = 50;
  srand(1828);

  callcount = 0;
  {
    boost::shared_ptr<MyActor> obj(new MyActor);

    qi::DynamicObjectBuilder builder;
    builder.setThreadingModel(qi::ObjectThreadingModel_SingleThread);
    builder.advertiseMethod("f",
        boost::function<void(int, qi::Promise<void>)>(boost::bind(&MyActor::f, obj, _1, _2)));
    builder.advertiseSignal("sig", &obj->sig);
    builder.advertiseProperty("prop", &obj->prop);

    qi::AnyObject aobj(builder.object());

    qi::Promise<void> finished;

    for (int i = 0; i < 25; ++i)
      aobj.async<void>("f", TOTAL, finished);
    for (int i = 0; i < 50; ++i)
      aobj.setProperty("prop", rand());
    QI_EMIT obj->sig(TOTAL);
    // we need one more call (the second test expects a periodic task to run at
    // least once)
    for (int i = 0; i < 26; ++i)
      aobj.async<void>("f", TOTAL, finished);
    finished.future().wait();
  }
  ASSERT_EQ(TOTAL + 1, callcount);
}

void chaincall(qi::AnyObject aobj, qi::Promise<void> finished, int TOTAL)
{
  for (int i = 0; i < 50; ++i)
    EXPECT_EQ(42, aobj.call<int>("f", TOTAL, finished));
}

TEST(TestStrand, AllFutureSignalPropertyPeriodicTaskAsyncCallTypeErased)
{
  static const int TOTAL = 300;
  srand(1828);

  callcount = 0;
  {
    boost::shared_ptr<MyActor> obj(new MyActor);
    qi::AnyObject aobj(obj);

    qi::Promise<void> finished;

    qi::PeriodicTask per;
    per.setUsPeriod(30);
    per.setCallback(&MyActor::f, obj.get(), TOTAL, finished);

    qi::Promise<void> prom;
    qi::Signal<void> signal;
    for (int i = 0; i < 50; ++i)
      prom.future().connect(&MyActor::f, obj.get(), TOTAL, finished);
    for (int i = 0; i < 50; ++i)
      signal.connect(&MyActor::f, obj.get(), TOTAL, finished);
    for (int i = 0; i < 50; ++i)
      aobj.connect("sig", obj->strand()->schedulerFor<void(int)>(&MyActor::f, obj, _1, finished));

    per.start();
    for (int i = 0; i < 25; ++i)
      aobj.async<void>("f", TOTAL, finished);
    for (int i = 0; i < 25; ++i)
      qi::async<void>(&MyActor::f, obj, TOTAL, finished);
    for (int i = 0; i < 50; ++i)
      aobj.setProperty("prop", rand());
    qi::Future<void> f = qi::async<void>(boost::bind(chaincall, aobj, finished, TOTAL));
    prom.setValue(0);
    QI_EMIT signal();
    QI_EMIT obj->sig(TOTAL);
    for (int i = 0; i < 25; ++i)
      aobj.async<void>("f", TOTAL, finished);
    for (int i = 0; i < 25; ++i)
      qi::async<void>(&MyActor::f, obj, TOTAL, finished);
    f.wait();
    finished.future().wait();
  }
  ASSERT_LT(TOTAL, callcount);
}

struct MyActorTrackable : MyActor, qi::Trackable<MyActorTrackable>
{
  MyActorTrackable() : Trackable(this) {}
  ~MyActorTrackable() { destroy(); }
};

TEST(TestStrand, FutureWithTrackable)
{
  callcount = 0;
  qi::Promise<void> prom;
  {
    qi::Promise<void> stub;
    MyActorTrackable obj;
    for (int i = 0; i < 10; ++i)
      prom.future().connect(&MyActorTrackable::f, &obj, 0, stub);
  }
  prom.setValue(0);
  ASSERT_EQ(0, callcount);
}

TEST(TestStrand, SignalWithTrackable)
{
  callcount = 0;
  qi::Signal<void> signal;
  {
    qi::Promise<void> stub;
    MyActorTrackable obj;
    for (int i = 0; i < 10; ++i)
      signal.connect(&MyActorTrackable::f, &obj, 0, stub);
  }
  signal();
  ASSERT_EQ(0, callcount);
}

int main(int argc, char* argv[])
{
  qi::Application app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  // better for these tests, since a task is 5ms long
  qi::os::setenv("QI_STRAND_QUANTUM_US", "50000");
  return RUN_ALL_TESTS();
}
