/*
** Author(s):
**  - Guillaume OREAL <goreal@aldebaran-robotics.com>
**
** Copyright (C) 2010, 2012 Aldebaran Robotics
*/

#include <vector>
#include <iostream>
#include <string>

#include <boost/thread/thread.hpp>
#include <qi/session.hpp>
#include <qi/anyobject.hpp>
#include <qi/type/dynamicobjectbuilder.hpp>
#include <qi/messaging/gateway.hpp>
#include <qi/os.hpp>
#include <qi/application.hpp>
#include <testsession/testsessionpair.hpp>

static std::string reply(const std::string &msg)
{
  qi::os::msleep(300);
  std::cout << msg << std::endl;
  return msg;
}

void myCall(qi::AnyObject myService)
{
  try
  {
    myService.call<std::string>("reply::s(s)", "ok");
    qi::os::msleep(300);
  }
  catch(std::exception e)
  {
    std::cout << e.what() << std::endl;
  }
}

//TEST(QiSession, Services)
void test(void)
{
  qi::Session sd;
  qi::Future<void> f = sd.listenStandalone("tcp://0.0.0.0:0");
  int timeToWait = 1;

  for(int j = 0; j < 10 ; j ++)
  {
    for (int i = 0; i < 20; i++)
    {
      TestSessionPair p;
      std::cout << "time to wait is:" << timeToWait << std::endl;

      qi::SessionPtr s1 = p.server();
      qi::SessionPtr s2 = p.client();

      qi::DynamicObjectBuilder ob;
      ob.advertiseMethod("reply", &reply);
      qi::AnyObject obj(ob.object());

      s1->registerService("service1", obj);

      qi::AnyObject myService;
      myService = s2->service("service1");

      boost::thread myThread(boost::bind(&myCall, myService));

      qi::os::msleep(timeToWait);
      s1->close();
      qi::os::msleep(3);
    }
    timeToWait = timeToWait +1;
  }
}
int main(int argc, char **argv)
{
  qi::Application app(argc, argv);
  TestMode::forceTestMode(TestMode::Mode_SD);

  test();
  return EXIT_SUCCESS;

}
