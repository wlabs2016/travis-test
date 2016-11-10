#pragma once
/*
 * Copyright (c) 2012 Aldebaran Robotics. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the COPYING file.
 */

#ifndef _SRC_UTILS_HPP_
#define _SRC_UTILS_HPP_

# include <string>

std::string fsconcat(const std::string &p0,
                     const std::string &p1,
                     const std::string &p2 = "",
                     const std::string &p3 = "",
                     const std::string &p4 = "",
                     const std::string &p5 = "");

std::string randomstr(std::string::size_type sz);
std::wstring wrandomstr(std::wstring::size_type sz);

#endif  // _SRC_UTILS_HPP_
