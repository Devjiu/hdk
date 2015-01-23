#include "../FileMgr.h"

#include <cstdlib>
#include <ctime>
#include <vector>
#include <iostream>
#include <boost/filesystem.hpp>

#include "gtest/gtest.h"
#include <boost/timer/timer.hpp>
#include <math.h>

using namespace File_Namespace;
using namespace std;

void deleteData(const std::string &dirName) {
    boost::filesystem::remove_all(dirName);
}

void writeToBuffer(AbstractBuffer *buffer, const size_t numInts) {
    int * data = new int [numInts];
    for (size_t i = 0; i < numInts; ++i) {
        data[i] = i;
    }
    buffer -> write((int8_t *)data,numInts*sizeof(int),CPU_BUFFER,0);
    delete [] data;
}

GTEST_API_ int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(FileMgr, getFreePages)
{
    deleteData("data");
    FileMgr fm("data");
    std::vector<Page> freePages;
    size_t numPages = 2048;
    size_t pageSize = 4096;
    
    EXPECT_EQ(freePages.size(), 0);
    fm.requestFreePages(numPages, pageSize, freePages);
    EXPECT_EQ(freePages.size(), numPages);
    
}

TEST(FileMgr, getFreePage)
{
    deleteData("data");
    FileMgr fm("data");
    size_t pageSize = 1024796;
    Page page = fm.requestFreePage(pageSize);
    EXPECT_EQ(page.isValid(),true);
}

TEST(FileMgr, createChunk) {
    deleteData("data");
    FileMgr fm("data");
    ChunkKey chunkKey = {2,3,4,5};
    size_t pageSize = 4096;
    AbstractBuffer * chunk1 = fm.createChunk(chunkKey,pageSize);
    AbstractBuffer * chunk2 = fm.getChunk(chunkKey);
    EXPECT_EQ(chunk1,chunk2);
    // Creating same chunk again should fail
    try {
        fm.createChunk(chunkKey,pageSize);
        EXPECT_TRUE(1==2) << "Created two chunks with same chunk key";
    }
    catch (std::runtime_error &error) {
        string expectedErrorString ("Chunk already exists.");
        EXPECT_EQ(error.what(),expectedErrorString);
    }
}

TEST(FileMgr, deleteChunk) {
    deleteData("data");
    ChunkKey chunkKey1 = {2,3,4,5};
    ChunkKey chunkKey2 = {2,4,4,5};
    {
        FileMgr fm("data");
        size_t pageSize = 4096;
        fm.createChunk(chunkKey1,pageSize);
        AbstractBuffer *chunk = fm.getChunk(chunkKey1);
        writeToBuffer(chunk,4096);
        // Test 1: Try to delete chunk
        try {
            fm.deleteChunk(chunkKey1); // should succeed
            EXPECT_TRUE(1==1);
        }
        catch (std::runtime_error &error) {
            EXPECT_TRUE(1==2) << "Could not delete a chunk that does exist";
        }

        // Test 2: Try to get chunk after its deleted
        try {
            AbstractBuffer *chunk1 = fm.getChunk(chunkKey1);
            EXPECT_TRUE(1==2) << "getChunk succeeded on a chunk that was deleted";
        }
        catch (std::runtime_error &error) {
            string expectedErrorString ("Chunk does not exist.");
            EXPECT_EQ(error.what(),expectedErrorString);
        }

        // Test 3: Try to delete chunk that was never created
        try {
            fm.deleteChunk(chunkKey2);
            EXPECT_TRUE(1==2) << "Deleted chunk that was never created";
        }
        catch (std::runtime_error &error) {
            string expectedErrorString ("Chunk does not exist.");
            EXPECT_EQ(error.what(),expectedErrorString);
        }

        // Test 4: Try to delete chunk that had previously been deleted
        try {
            fm.deleteChunk(chunkKey1);
            EXPECT_TRUE(1==2) << "Deleted chunk that had already been deleted";
        }
        catch (std::runtime_error &error) {
            string expectedErrorString ("Chunk does not exist.");
            EXPECT_EQ(error.what(),expectedErrorString);
        }
        fm.checkpoint();
    }
    // Test 5: Destroy FileMgr and reinstanciate it to make sure it has no
    // trace of chunk with key chunkKey1
    {
        FileMgr fm("data");
        try {
            AbstractBuffer *chunk1 = fm.getChunk(chunkKey1);
            EXPECT_TRUE(1==2) << "getChunk succeeded on a chunk that was deleted after FileMgr reinstanciation";
        }
        catch (std::runtime_error &error) {
            string expectedErrorString ("Chunk does not exist.");
            EXPECT_EQ(error.what(),expectedErrorString);
        }
    }
}

TEST(FileMgr, writeReadChunk) {
    deleteData("data");
    ChunkKey chunkKey1 = {1,2,3,4};
    ChunkKey chunkKey2 = {2,3,4,5};
    size_t pageSize = 1024796;
    //size_t pageSize = 8192;
    //size_t pageSize = 4096000;
    FileMgr fm("data");
    fm.createChunk(chunkKey1,pageSize);
    fm.createChunk(chunkKey2,pageSize);
    const boost::timer::nanosecond_type oneSecond(1000000000LL);
    size_t numInts = 10000000;
    int * data1 = new int [numInts];
    for (size_t i = 0; i < numInts; ++i) {
        data1[i] = i;
    }
    AbstractBuffer *chunk1 = fm.getChunk(chunkKey1);
    {
        boost::timer::cpu_timer cpuTimer;
        chunk1 -> write((int8_t *)data1,numInts*sizeof(int),CPU_BUFFER,0);
        //cout << "Checkpoint 1" << endl;
        fm.checkpoint();
        double elapsedTime = double(cpuTimer.elapsed().wall) / oneSecond;
        double bandwidth = numInts * sizeof(int) / elapsedTime / 1000000000.0;
        cout << "Write Bandwidth with checkpoint: " << bandwidth << " GB/sec" << endl;
    }
    AbstractBuffer *chunk2 = fm.getChunk(chunkKey2);
    {
        boost::timer::cpu_timer cpuTimer;
        chunk1 -> write((int8_t *)data1,numInts*sizeof(int),CPU_BUFFER,0);
        double elapsedTime = double(cpuTimer.elapsed().wall) / oneSecond;
        double bandwidth = numInts * sizeof(int) / elapsedTime / 1000000000.0;
        cout << "Write Bandwidth without checkpoint: " << bandwidth << " GB/sec" << endl;
    }

    int * data2 = new int [numInts];
    {
        boost::timer::cpu_timer cpuTimer;
        chunk1 -> read((int8_t *)data2,numInts*sizeof(int),CPU_BUFFER,0);
        double elapsedTime = double(cpuTimer.elapsed().wall) / oneSecond;
        double bandwidth = numInts * sizeof(int) / elapsedTime / 1000000000.0;
        cout << "Read Bandwidth: " << bandwidth << " GB/sec" << endl;
    }

    for (size_t i = 0; i < numInts; ++i) {
        EXPECT_EQ(data1[i],data2[i]);
    }

    delete [] data1;
    delete [] data2;
}

TEST(FileMgr, persistence) {
    deleteData("data");
    ChunkKey chunkKey1 = {1,2,3,4};
    size_t pageSize = 1024796;
    size_t numInts = 1000000;
    int * data1 = new int [numInts];
    for (size_t i = 0; i < numInts; ++i) {
        data1[i] = i;
    }
    {
        FileMgr fm("data");
        fm.createChunk(chunkKey1,pageSize);
        AbstractBuffer *chunk1 = fm.getChunk(chunkKey1);
        chunk1 -> write((int8_t *)data1,numInts*sizeof(int),CPU_BUFFER,0);
        fm.checkpoint();
        // should checkpoint here 
    }
    FileMgr fm("data");
    AbstractBuffer * chunk1 = fm.getChunk(chunkKey1);
    EXPECT_EQ(chunk1 -> size(),numInts * sizeof(int));
    delete [] data1;
}

TEST(FileMgr, epochPersistence) {
    deleteData("data");
    ChunkKey chunkKey1 = {1,2,3,4};
    size_t pageSize = 1024796;
    size_t numInts = 100000;
    int * data1 = new int [numInts];
    for (size_t i = 0; i < numInts; ++i) {
        data1[i] = i;
    }
    {
        FileMgr fm("data");
        fm.createChunk(chunkKey1,pageSize);
        AbstractBuffer *chunk1 = fm.getChunk(chunkKey1);
        chunk1 -> append((int8_t *)data1,numInts*sizeof(int),CPU_BUFFER);
        fm.checkpoint(); // checkpoint 1
        chunk1 -> append((int8_t *)data1,numInts*sizeof(int),CPU_BUFFER);
        fm.checkpoint(); // checkpoint 2
        chunk1 -> append((int8_t *)data1,numInts*sizeof(int),CPU_BUFFER);
        fm.checkpoint(); // checkpoint 3
        chunk1 -> append((int8_t *)data1,numInts*sizeof(int),CPU_BUFFER);
        fm.checkpoint(); // checkpoint 4
    }



    {
        FileMgr fm("data");
        AbstractBuffer * chunk1 = fm.getChunk(chunkKey1);
        EXPECT_EQ(chunk1 -> size(),4*numInts * sizeof(int));
    }
    {
        FileMgr fm("data", 1024796, 3);
        AbstractBuffer * chunk1 = fm.getChunk(chunkKey1);
        EXPECT_EQ(chunk1 -> size(),3*numInts * sizeof(int));
    }
    {
        FileMgr fm("data", 1024796, 2);
        AbstractBuffer * chunk1 = fm.getChunk(chunkKey1);
        EXPECT_EQ(chunk1 -> size(),2*numInts * sizeof(int));
    }
    {
        FileMgr fm("data", 1024796, 1);
        /*
        AbstractBuffer * chunk1 = fm.getChunk(chunkKey1);
        EXPECT_EQ(chunk1 -> size(),1*numInts * sizeof(int));
        */
    }
}

TEST(FileMgr, encoding) {
    deleteData("data");
    ChunkKey chunkKey1 = {1,2,3,4};
    ChunkKey chunkKey2 = {5,6,7,8};
    int numElems = 10000;
    int * data1 = new int [numElems];
    int * data2 = new int [numElems];
    //float * data2 = new float [numElems];
    for (size_t i = 0; i < numElems; ++i) {
        data1[i] = i % 100; // so fits in one byte
        //data2[i] = M_PI * i;
        data2[i] = i % 100 * -1;
    }
    {
        FileMgr fm("data");
        //size_t pageSize = 1024796;
        size_t pageSize = 8192;
        AbstractBuffer * chunk1 =  fm.createChunk(chunkKey1,pageSize);
        chunk1 -> initEncoder(kINT,kENCODING_FIXED,8);
        EXPECT_EQ(kINT,chunk1->sqlType);
        EXPECT_EQ(kENCODING_FIXED,chunk1->encodingType);
        EXPECT_EQ(8,chunk1->encodingBits);


        int8_t * tmpPtr = reinterpret_cast <int8_t *> (data1);
        chunk1 -> encoder -> appendData(tmpPtr,numElems);
        EXPECT_EQ(numElems,chunk1 -> size());
        EXPECT_EQ(numElems,chunk1 -> encoder -> numElems);
        AbstractBuffer * chunk2 =  fm.createChunk(chunkKey2,pageSize);

    /*        
        chunk2 -> initEncoder(kINT,kENCODING_FIXED,kINT8);
        //chunk2 -> initEncoder(k,kENCODING_NONE,kNONE);
        EXPECT_EQ(kINT,chunk2->sqlType);
        EXPECT_EQ(kENCODING_FIXED,chunk2->encodingType);
        EXPECT_EQ(kINT8,chunk2->encodedDataType);
        cout << "After chunk2 encoder init" << endl;
        chunk2 -> encoder -> appendData((int8_t *)data2,numElems);
        cout << "After chunk2 append" << endl;
        EXPECT_EQ(numElems,chunk2 -> size());
        EXPECT_EQ(numElems,chunk2 -> encoder -> numElems);
        fm.checkpoint();
    }

    {
        FileMgr fm("data");
        cout << "After new file manager" << endl;
        AbstractBuffer * chunk1 =  fm.getChunk(chunkKey1);
        EXPECT_EQ(kINT,chunk1->sqlType);
        EXPECT_EQ(kENCODING_FIXED,chunk1->encodingType);
        EXPECT_EQ(kINT8,chunk1->encodedDataType);
        EXPECT_EQ(numElems,chunk1 -> size());
        EXPECT_EQ(numElems,chunk1 -> encoder -> numElems);
        chunk1 -> encoder -> appendData((int8_t *)data1,numElems);
        EXPECT_EQ(numElems*2,chunk1 -> size());
        EXPECT_EQ(numElems*2,chunk1 -> encoder -> numElems);
        cout << "After done with chunk1" << endl;

        AbstractBuffer * chunk2= fm.getChunk(chunkKey2);
        EXPECT_EQ(kINT,chunk2->sqlType);
        EXPECT_EQ(kENCODING_FIXED,chunk2->encodingType);
        EXPECT_EQ(kINT8,chunk2->encodedDataType);
        EXPECT_EQ(numElems,chunk2 -> size());
        EXPECT_EQ(numElems,chunk2 -> encoder -> numElems);
        chunk2 -> encoder -> appendData((int8_t *)data2,numElems);
        EXPECT_EQ(2*numElems,chunk2 -> size());
        EXPECT_EQ(2*numElems,chunk2 -> encoder -> numElems);

    }
    */

        
        chunk2 -> initEncoder(kINT,kENCODING_NONE);
        //chunk2 -> initEncoder(k,kENCODING_NONE,kNONE);
        EXPECT_EQ(kINT,chunk2->sqlType);
        EXPECT_EQ(kENCODING_NONE,chunk2->encodingType);
        //cout << "After chunk2 encoder init" << endl;
        tmpPtr = reinterpret_cast <int8_t *> (data2);
        chunk2 -> encoder -> appendData(tmpPtr,numElems);
        //cout << "After chunk2 append" << endl;
        EXPECT_EQ(numElems*sizeof(float),chunk2 -> size());
        EXPECT_EQ(numElems,chunk2 -> encoder -> numElems);
        fm.checkpoint();
    }

    {
        FileMgr fm("data");
        //cout << "After new file manager" << endl;
        AbstractBuffer * chunk1 =  fm.getChunk(chunkKey1);
        EXPECT_EQ(kINT,chunk1->sqlType);
        EXPECT_EQ(kENCODING_FIXED,chunk1->encodingType);
        EXPECT_EQ(8,chunk1->encodingBits);
        EXPECT_EQ(numElems,chunk1 -> size());
        EXPECT_EQ(numElems,chunk1 -> encoder -> numElems);
        int8_t * tmpPtr = reinterpret_cast <int8_t *> (data1);
        chunk1 -> encoder -> appendData(tmpPtr,numElems);
        EXPECT_EQ(numElems*2,chunk1 -> size());
        EXPECT_EQ(numElems*2,chunk1 -> encoder -> numElems);
        //cout << "After done with chunk1" << endl;
        tmpPtr = reinterpret_cast <int8_t *> (data1);
        chunk1 -> encoder -> appendData(tmpPtr,numElems);
        tmpPtr = reinterpret_cast <int8_t *> (data1);
        chunk1 -> encoder -> appendData(tmpPtr,numElems);
        tmpPtr = reinterpret_cast <int8_t *> (data1);
        chunk1 -> encoder -> appendData(tmpPtr,numElems);

        AbstractBuffer * chunk2= fm.getChunk(chunkKey2);
        EXPECT_EQ(kINT,chunk2->sqlType);
        EXPECT_EQ(kENCODING_NONE,chunk2->encodingType);
        EXPECT_EQ(numElems*sizeof(float),chunk2 -> size());
        EXPECT_EQ(numElems,chunk2 -> encoder -> numElems);
        tmpPtr = reinterpret_cast <int8_t *> (data2);
        chunk2 -> encoder -> appendData(tmpPtr,numElems);
        EXPECT_EQ(2*numElems*sizeof(float),chunk2 -> size());
        EXPECT_EQ(2*numElems,chunk2 -> encoder -> numElems);
        fm.checkpoint();

    }

}
