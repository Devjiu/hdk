g++ -g --std=c++0x -o FileMgrTest FileMgrTest.cpp ../FileMgr.cpp ../File.cpp ../FileBuffer.cpp ../FileInfo.cpp ../../Encoder.cpp  -I/usr/local/include -L/usr/local/lib -lgtest -lboost_filesystem-mt -lboost_system-mt -lboost_timer-mt
