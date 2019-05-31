/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Runs client tests in their own thread inside a WSD process.

#include <config.h>

#include <random>

#include <Exceptions.hpp>
#include <Log.hpp>
#include <Unit.hpp>
#include <UnitHTTP.hpp>
#include <helpers.hpp>
#include <LOOLWSD.hpp>

#include <wsd/TileDesc.hpp>

using namespace ::helpers;

// Inside the WSD process
class UnitTyping : public UnitWSD
{
    bool _workerStarted;
    std::thread _worker;

public:
    UnitTyping() :
        _workerStarted(false)
    {
        int timeout_minutes = 5;
        setTimeout(timeout_minutes * 60 * 1000);
    }
    ~UnitTyping()
    {
        LOG_INF("Joining test worker thread\n");
        _worker.join();
    }

    bool filterAlertAllusers(const std::string & msg) override
    {
        std::cout << "Alert: " << msg << "\n";
        return false;
    }


    TestResult testWriterTyping()
    {
//        const char* testname = "writerMultiViewEdit ";
        return TestResult::Ok;
    }

    TestResult testCalcTyping()
    {
        const char* testname = "calcMultiViewEdit ";
        std::string serverURL = LOOLWSD::getServerURL();
        const Poco::URI uri(serverURL);

        // Load a doc with the cursor saved at a top row.
        std::string documentPath, documentURL;
        helpers::getDocumentPathAndURL("empty.ods", documentPath, documentURL, testname);

        const int numRender = 2;
        const int numTyping = 6;
        const int numSocket = numRender + numTyping;
        std::vector<std::shared_ptr<LOOLWebSocket>> sockets;

        LOG_TRC("Connecting first client to " << serverURL << " doc: " << documentURL);
        sockets.push_back(helpers::loadDocAndGetSocket(uri, documentURL, testname));

        for (int i = 1; i < numSocket; ++i)
        {
            LOG_TRC("Connecting client " << i);
            std::shared_ptr<LOOLWebSocket> socket = helpers::loadDocAndGetSocket(uri, documentURL, testname);
            sockets.push_back(socket);
            for (int j = 0; j < i * 3; ++j)
            {
                // cursor down some multiple of times
                sendTextFrame(socket, "key type=input char=0 key=1024", testname);
                sendTextFrame(socket, "key type=up char=0 key=1024", testname);
                assertResponseString(socket, "celladdress:", testname);
            }
        }

        int count = 100 * numTyping;
        std::vector<std::string> messages[numTyping];

        // setup.
        for (int i = 0; i < numTyping; ++i)
        {
            messages[i].push_back("clientvisiblearea x=0 y=0 width=27960 height=5160");
            messages[i].push_back("clientzoom tilepixelwidth=256 tilepixelheight=256 tiletwipwidth=3840 tiletwipheight=3840");
            messages[i].push_back("commandvalues command=.uno:ViewRowColumnHeaders?x=-15&y=3870&width=0&height=5160");
            messages[i].push_back("useractive");
        }

        // randonly queue 'a', 'b' etc. key-press / space to each socket.
        std::mt19937 randMt(0);
        for (int i = 0; i < count; ++i)
        {
            int which = i % numTyping;
            std::vector<std::string> &msgs = messages[which];
            int chr = 97 + which;
            int key = 512 + which * 2;

            bool bSpace = !(randMt() & 0300); // send a space

            msgs.push_back("key type=input char=" + std::to_string(chr) + " key=0");
            msgs.push_back("key type=up char=0 key=" + std::to_string(key));
            if (bSpace)
            {
                msgs.push_back("key type=input char=32 key=0");
                msgs.push_back("key type=up char=0 key=1284");
            }
        }

        int waitMS = 5;
        std::vector<std::thread> threads;
        std::atomic<bool> started(false);
        std::atomic<int> liveTyping(0);

        // First some rendering load
        threads.reserve(numRender);
        for (int i = 0; i < numRender; ++i)
            threads.emplace_back([&,i] {
                    std::mt19937 randDev(numRender * 257);
                    std::shared_ptr<LOOLWebSocket> sock = sockets[numTyping + i];
                    while (!started || liveTyping > 0)
                    {
                        std::ostringstream oss;
                        std::uniform_int_distribution<int> distribution(0,32);
                        oss << "tilecombine part=0 width=512 height=512"
                            << " tileposx=" << 3840*distribution(randDev)
                            << " tileposy=" << 3840*distribution(randDev)
                            << " tilewidth=7680 tileheight=7680";
                        sendTextFrame(sock, oss.str(), testname);

                        std::vector<char> tile = getResponseMessage(sock, "tile:", testname, 5 /* ms */);

                        std::this_thread::sleep_for(std::chrono::milliseconds(25));
                    }
                });

        // Add some typing
        for (int which = 0; which < numTyping; ++which)
        {
            threads.emplace_back([&,which] {
                    std::mt19937 randDev(which * 16);
                    std::shared_ptr<LOOLWebSocket> sock = sockets[which];
                    liveTyping++;
                    started = true;
                    for (size_t i = 0; i < messages[which].size(); ++i)
                    {
                        std::string msg = messages[which][i];

                        std::uint_fast32_t num = randDev();
                        if (!(num & 0x30))
                            sendTextFrame(sock, "ping", testname);

                        // suck and dump replies down
                        std::vector<char> tile = getResponseMessage(sock, "tile:", testname, waitMS /* ms */);
                        if (tile.size())
                        {
// 1544818858022 INCOMING: tile: part=0 width=256 height=256 tileposx=15360 tileposy=38400 tilewidth=3840 tileheight=3840 oldwid=0 wid=232 ver=913 imgsize=1002
// Socket.js:123 1544818858027 OUTGOING: tileprocessed tile=0:15360:38400:3840:3840
                            TileDesc desc = TileDesc::parse(LOOLProtocol::tokenize(tile.data(), tile.size()));
                            sendTextFrame(sock, "tileprocessed tile=" + desc.generateID(), testname);
                        }

                        if (!(num & 0x300)) // occasionally sleep some more - why not.
                            std::this_thread::sleep_for(std::chrono::milliseconds(waitMS*25));
                    LOG_TRC("Send to " << which << " message " << msg);
//                    std::cout << "Send to " << which << " message " << msg << "\n";
                        sendTextFrame(sock, msg, testname);
                    }
                    liveTyping--;
                });
        }

        for (auto& thread : threads)
        {
            thread.join();
        }

        // complete the cells with some 'enters'
        std::string results[numTyping];
        for (int i = 0; i < numTyping; ++i)
        {
            sendTextFrame(sockets[i], "key type=input char=13 key=0", testname);
            sendTextFrame(sockets[i], "key type=up char=0 key=1280", testname);

            // extract their text
            sendTextFrame(sockets[i], "uno .uno:SelectAll", testname);
            sendTextFrame(sockets[i], "gettextselection mimetype=text/plain;charset=utf-8", testname);

            LOG_TRC("Waiting for test selection:");
            std::string result = getResponseString(sockets[i],  "textselectioncontent:", testname, 20000 /* 20 secs */);
            results[i] = result;

            char target = 'a'+i;
            LOG_TRC("Result [" << i << "] target " << target << " is '" << result << "'");
            for (size_t j = sizeof("textselectioncontent:"); j < result.size(); ++j)
            {
                if (result[j] != ' ' && result[j] != target)
                {
                    LOG_TRC("Text contains incorrect char[" << j << "] = '" << result[j] << "' not " << target << " '" << result << "'");
                    if (result[j] != target)
                        return TestResult::Failed;
                }
            }
        }

        return TestResult::Ok;
    }

    TestResult testTyping()
    {
        TestResult res;
        res = testWriterTyping();
        if (res != TestResult::Ok)
            return res;
//        res = testCalcTyping();
        return res;
    }

    void invokeTest() override
    {
        // this method gets called every few seconds.
        if (_workerStarted)
            return;
        _workerStarted = true;

        _worker = std::thread([this]{
                exitTest (testTyping());
            });
    }
};

UnitBase *unit_create_wsd(void)
{
    return new UnitTyping();
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
