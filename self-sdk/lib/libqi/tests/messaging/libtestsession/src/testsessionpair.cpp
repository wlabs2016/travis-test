/*
**
** Author(s):
**  - Pierre Roullon <proullon@aldebaran-robotics.com>
**
** Copyright (C) 2012 Aldebaran Robotics
*/

#include <qi/os.hpp>

#include <testsession/testsession.hpp>
#include <testsession/testsessionpair.hpp>

TestSessionPair::TestSessionPair(TestMode::Mode mode, const std::string sdUrl)
{
  _sd = qi::makeSession();

  // #1 Get active mode.
  _mode = mode == TestMode::Mode_Default ? TestMode::getTestMode() : mode;

  // #2 Listen.
  if (_mode == TestMode::Mode_SSL)
  {
    _sd->setIdentity("../tests/server.key", "../tests/server.crt");
    _sd->listenStandalone("tcps://0.0.0.0:0");
  }
  else
  {
    _sd->listenStandalone(sdUrl);
  }

  // #3 Get client and server sessions.
  _client = new TestSession(_sd->endpoints()[0].str(), false, _mode);
  _server = new TestSession(_sd->endpoints()[0].str(), true, _mode);
}

TestSessionPair::TestSessionPair(TestSessionPair &other)
{
  _sd = qi::makeSession();

  // #1 Get active mode.
  _mode = TestMode::getTestMode();

  // #2 Get client and server sessions using other pair service directory.
  _client = new TestSession(other._sd->endpoints()[0].str(), false, _mode);
  _server = new TestSession(other._sd->endpoints()[0].str(), true, _mode);
}

TestSessionPair::~TestSessionPair()
{
  delete _client;
  delete _server;
}

qi::SessionPtr TestSessionPair::client() const
{
  // #0 If activated test mode is 'Direct', cheat.
  if (_mode == TestMode::Mode_Direct)
    return _server->session();

  return _client->session();
}

qi::SessionPtr TestSessionPair::server() const
{
  return _server->session();
}

qi::SessionPtr TestSessionPair::sd() const
{
  return _sd;
}

std::vector<qi::Url> TestSessionPair::serviceDirectoryEndpoints() const
{
  return _sd->endpoints();
}
