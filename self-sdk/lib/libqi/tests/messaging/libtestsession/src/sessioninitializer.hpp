/*
**
** Author(s):
**  - Pierre Roullon <proullon@aldebaran-robotics.com>
**
** Copyright (C) 2012 Aldebaran Robotics
*/

/*!
 * \internal
 * \class SessionInitializer
 * \brief Initialize elements (Gateways, sessions, services...) needed to suit required test mode.
 * \since 1.18
 * \author Pierre Roullon
 */

#ifndef _TESTS_LIBTESTSESSION_SESSIONINITIALIZER_HPP_
#define _TESTS_LIBTESTSESSION_SESSIONINITIALIZER_HPP_

#include <map>
#include <testsession/testsession.hpp>

#include "populationgenerator.hpp"
#include "trafficgenerator.hpp"

class SessionInitializer
{
public:
  SessionInitializer();
  ~SessionInitializer();

public:
  bool setUp(qi::SessionPtr session, const std::string &serviceDirectoryUrl, TestMode::Mode mode, bool listen);
  bool tearDown(qi::SessionPtr session, TestMode::Mode mode);

private:
  bool setUpSD(qi::SessionPtr session, const std::string &serviceDirectoryUrl);
  bool setUpSSL(qi::SessionPtr session, const std::string &serviceDirectoryUrl);
  bool setUpNightmare(qi::SessionPtr session, const std::string &serviceDirectoryUrl);
  bool tearDownSD(qi::SessionPtr session);
  bool tearDownNightmare(qi::SessionPtr session);

private:
  typedef bool (SessionInitializer::*setUpFcnt)(qi::SessionPtr session, const std::string &serviceDirectoryUrl);
  typedef bool (SessionInitializer::*tearDownFcnt)(qi::SessionPtr session);

  bool                                     _listen;

  std::map<TestMode::Mode, setUpFcnt>      _setUps;
  std::map<TestMode::Mode, tearDownFcnt>   _tearDowns;

  PopulationGenerator                     *_populationGenerator;
  TrafficGenerator                        *_trafficGenerator;
};

#endif // !_TESTS_LIBTESTSESSION_SESSIONINITIALIZER_HPP_
