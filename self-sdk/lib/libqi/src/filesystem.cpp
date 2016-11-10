/*
 * Copyright (c) 2012 Aldebaran Robotics. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the COPYING file.
 */

#include <numeric>
#include <boost/filesystem.hpp>
#include <qi/path.hpp>
#include "filesystem.hpp"

namespace qi
{
  namespace detail
  {

    static boost::filesystem::path normalize(boost::filesystem::path path1,
                                             boost::filesystem::path path2)
    {
      if (*path2.begin() == ".")
        return path1;
      if (*path2.begin() == "..")
        return path1.parent_path();
      else
        return path1 /= path2;
    }

    std::string normalizePath(const std::string& path)
    {
      boost::filesystem::path p(path, qi::unicodeFacet());
      p = std::accumulate(p.begin(), p.end(), boost::filesystem::path(), normalize);
      return p.make_preferred().string(qi::unicodeFacet());
    }

  }
} // namesapce qi::detail
