#! /bin/bash
set -e
echo 'Installing qiBuild!!!!'
ruby -e "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)"
#brew install cmake
pip install qibuild
cd ../self-sdk/
./scripts/build_mac.sh mac

