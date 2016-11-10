/*
**
** Author(s):
**  - Pierre Roullon <proullon@aldebaran-robotics.com>
**
** Copyright (C) 2012 Aldebaran Robotics
*/

#include "defaultservice.hpp"
#include "populationgenerator.hpp"
#include "sessioninitializer.hpp"

SessionInitializer::SessionInitializer() :
  _listen(false),
  _populationGenerator(0),
  _trafficGenerator(0)
{
  _setUps[TestMode::Mode_SD] = &SessionInitializer::setUpSD;
  _setUps[TestMode::Mode_SSL] = &SessionInitializer::setUpSSL;
  _setUps[TestMode::Mode_Direct] = &SessionInitializer::setUpSD;
  _setUps[TestMode::Mode_Nightmare] = &SessionInitializer::setUpNightmare;

  _tearDowns[TestMode::Mode_SD] = &SessionInitializer::tearDownSD;
  _tearDowns[TestMode::Mode_SSL] = &SessionInitializer::tearDownSD;
  _tearDowns[TestMode::Mode_Direct] = &SessionInitializer::tearDownSD;
  _tearDowns[TestMode::Mode_Nightmare] = &SessionInitializer::tearDownNightmare;
}

SessionInitializer::~SessionInitializer()
{
}

bool SessionInitializer::setUp(qi::SessionPtr session, const std::string &serviceDirectoryUrl, TestMode::Mode mode, bool listen)
{
  if (_setUps.find(mode) == _setUps.end())
    throw TestSessionError("[Internal] setUp mode not handled.");

  _listen = listen;
  return (this->*_setUps[mode])(session, serviceDirectoryUrl);
}

bool SessionInitializer::tearDown(qi::SessionPtr session, TestMode::Mode mode)
{
  if (_tearDowns.find(mode) == _tearDowns.end())
    throw TestSessionError("[Internal] tearDown mode not handled.");

  return (this->*_tearDowns[mode])(session);
}

bool SessionInitializer::setUpSD(qi::SessionPtr session, const std::string &serviceDirectoryUrl)
{
  session->connect(serviceDirectoryUrl);

  if (_listen == true)
    session->listen("tcp://0.0.0.0:0");

  return true;
}

bool SessionInitializer::setUpSSL(qi::SessionPtr session, const std::string &serviceDirectoryUrl)
{
  if(session->connect(serviceDirectoryUrl).wait(1000) != qi::FutureState_FinishedWithValue)
    return false;

  if (_listen == true)
  {
    session->setIdentity("../tests/server.key", "../tests/server.crt");
    session->listen("tcps://0.0.0.0:0");
  }

  return true;
}

bool SessionInitializer::tearDownSD(qi::SessionPtr session)
{
  if (session->close().wait(1000) != qi::FutureState_FinishedWithValue)
    return false;

  return true;
}

bool SessionInitializer::setUpNightmare(qi::SessionPtr session, const std::string &serviceDirectoryUrl)
{
  std::string serviceName;

  // #1 Connect session to service directory.
  if(session->connect(serviceDirectoryUrl).wait(1000) != qi::FutureState_FinishedWithValue)
    return false;

  // #1.1 If session is a client session, that's it.
  if (_listen == false)
    return true;

  // #1.2 Make session listen.
  session->listen("tcp://0.0.0.0:0");

  // #2 Allocate population and traffic tools.
  _populationGenerator = new PopulationGenerator();
  _trafficGenerator = new TrafficGenerator();

  // #3 Generate an unique name for hidder service
  if (DefaultService::generateUniqueServiceName(serviceName) == false)
    throw TestSessionError("[Internal] Cannot generate unique service name.");

  // #4 Register hidden service.
  session->registerService(serviceName, DefaultService::getDefaultService());

  // #5 Populate with client session and generate traffic.
  if (_populationGenerator->populateClients(serviceDirectoryUrl, 10000) == false)
    return false;

  if (_trafficGenerator->generateCommonTraffic(_populationGenerator->clientPopulation(), serviceName) == false)
    return false;

  return true;
}

bool SessionInitializer::tearDownNightmare(qi::SessionPtr session)
{
  if (_trafficGenerator)
    _trafficGenerator->stopTraffic();

  delete _populationGenerator;
  delete _trafficGenerator;

  if (session->close().wait(1000) != qi::FutureState_FinishedWithValue)
    return false;

  return true;
}
