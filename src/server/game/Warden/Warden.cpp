/*
 * Copyright (C)
 * Copyright (C)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Common.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Log.h"
#include "Opcodes.h"
#include "ByteBuffer.h"
#include <openssl/md5.h>
#include <openssl/sha.h>
#include "World.h"
#include "Player.h"
#include "Util.h"
#include "Warden.h"
#include "AccountMgr.h"

Warden::Warden() : _session(NULL), _inputCrypto(16), _outputCrypto(16), _checkTimer(10000/*10 sec*/), _clientResponseTimer(0),
                   _dataSent(false), _previousTimestamp(0), _module(NULL), _initialized(false)
{
    memset(_inputKey, 0, sizeof(_inputKey));
    memset(_outputKey, 0, sizeof(_outputKey));
    memset(_seed, 0, sizeof(_seed));
}

Warden::~Warden()
{
    delete[] _module->CompressedData;
    delete _module;
    _module = NULL;
    _initialized = false;
}

void Warden::SendModuleToClient()
{
    ;//sLog->outDebug(LOG_FILTER_WARDEN, "Send module to client");

    // Create packet structure
    WardenModuleTransfer packet;

    uint32 sizeLeft = _module->CompressedSize;
    uint32 pos = 0;
    uint16 burstSize;
    while (sizeLeft > 0)
    {
        burstSize = sizeLeft < 500 ? sizeLeft : 500;
        packet.Command = WARDEN_SMSG_MODULE_CACHE;
        packet.DataSize = burstSize;
        memcpy(packet.Data, &_module->CompressedData[pos], burstSize);
        sizeLeft -= burstSize;
        pos += burstSize;

        EncryptData((uint8*)&packet, burstSize + 3);
        WorldPacket pkt1(SMSG_WARDEN_DATA, burstSize + 3);
        pkt1.append((uint8*)&packet, burstSize + 3);
        _session->SendPacket(&pkt1);
    }
}

void Warden::RequestModule()
{
    ;//sLog->outDebug(LOG_FILTER_WARDEN, "Request module");

    // Create packet structure
    WardenModuleUse request;
    request.Command = WARDEN_SMSG_MODULE_USE;

    memcpy(request.ModuleId, _module->Id, 16);
    memcpy(request.ModuleKey, _module->Key, 16);
    request.Size = _module->CompressedSize;

    // Encrypt with warden RC4 key.
    EncryptData((uint8*)&request, sizeof(WardenModuleUse));

    WorldPacket pkt(SMSG_WARDEN_DATA, sizeof(WardenModuleUse));
    pkt.append((uint8*)&request, sizeof(WardenModuleUse));
    _session->SendPacket(&pkt);
}

void Warden::Update()
{
    if (_initialized)
    {
        uint32 currentTimestamp = World::GetGameTimeMS();
        uint32 diff = getMSTimeDiff(_previousTimestamp, currentTimestamp);
        _previousTimestamp = currentTimestamp;

        if (_dataSent)
        {
            uint32 maxClientResponseDelay = sWorld->getIntConfig(CONFIG_WARDEN_CLIENT_RESPONSE_DELAY);
            if (maxClientResponseDelay > 0)
            {
                if (_clientResponseTimer > maxClientResponseDelay * IN_MILLISECONDS)
                    _session->KickPlayer();
                else
                    _clientResponseTimer += diff;
            }
        }
        else
        {
            if (diff >= _checkTimer)
                RequestData();
            else
                _checkTimer -= diff;
        }
    }
}

void Warden::DecryptData(uint8* buffer, uint32 length)
{
    _inputCrypto.UpdateData(length, buffer);
}

void Warden::EncryptData(uint8* buffer, uint32 length)
{
    _outputCrypto.UpdateData(length, buffer);
}

bool Warden::IsValidCheckSum(uint32 checksum, const uint8* data, const uint16 length)
{
    uint32 newChecksum = BuildChecksum(data, length);

    if (checksum != newChecksum)
    {
        ;//sLog->outDebug(LOG_FILTER_WARDEN, "CHECKSUM IS NOT VALID");
        return false;
    }
    else
    {
        ;//sLog->outDebug(LOG_FILTER_WARDEN, "CHECKSUM IS VALID");
        return true;
    }
}

struct keyData {
    union
    {
        struct
        {
            uint8 bytes[20];
        } bytes;

        struct
        {
            uint32 ints[5];
        } ints;
    };
};

uint32 Warden::BuildChecksum(const uint8* data, uint32 length)
{
    keyData hash;
    SHA1(data, length, hash.bytes.bytes);
    uint32 checkSum = 0;
    for (uint8 i = 0; i < 5; ++i)
        checkSum = checkSum ^ hash.ints.ints[i];

    return checkSum;
}

std::string Warden::Penalty(WardenCheck* check /*= NULL*/, uint16 checkFailed /*= 0*/)
{
    WardenActions action;

    if (check)
        action = check->Action;
    else
        action = WardenActions(sWorld->getIntConfig(CONFIG_WARDEN_CLIENT_FAIL_ACTION));

    std::string banReason = "Anticheat violation";
    bool longBan = false; // 14d = 1209600s
    if (checkFailed)
        switch (checkFailed)
        {
            case 47: banReason += " (FrameXML Signature Check)"; break;
            case 51: banReason += " (Lua DoString)"; break;
            case 59: banReason += " (Lua Protection Patch)"; break;
            case 72: banReason += " (Movement State related)"; break;
            case 118: banReason += " (Wall Climb)"; break;
            case 121: banReason += " (No Fall Damage Patch)"; break;
            case 193: banReason += " (Follow Unit Check)"; break;
            case 209: banReason += " (WoWEmuHacker Injection)"; longBan = true; break;
            case 237: banReason += " (AddChatMessage)"; break;
            case 246: banReason += " (Language Patch)"; break;
            case 260: banReason += " (Jump Momentum)"; break;
            case 288: banReason += " (Language Patch)"; break;
            case 308: banReason += " (SendChatMessage)"; break;
            case 312: banReason += " (Jump Physics)"; break;
            case 314: banReason += " (GetCharacterInfo)"; break;
            case 329: banReason += " (Wall Climb)"; break;
            case 343: banReason += " (Login Password Pointer)"; break;
            case 349: banReason += " (Language Patch)"; break;
            case 712: banReason += " (WS2_32.Send)"; break;
            case 780: banReason += " (Lua Protection Remover)"; break;
            case 781: banReason += " (Walk on Water Patch)"; break;
            case 782: banReason += " (Collision M2 Special)"; longBan = true; break;
            case 783: banReason += " (Collision M2 Regular)"; longBan = true; break;
            case 784: banReason += " (Collision WMD)"; longBan = true; break;
            case 785: banReason += " (Multi-Jump Patch)"; break;
            case 786: banReason += " (WPE PRO)"; longBan = true; break;
            case 787: banReason += " (rEdoX Packet Editor)"; break;
        }

    switch (action)
    {
    case WARDEN_ACTION_LOG:
        return "None";
        break;
    case WARDEN_ACTION_KICK:
        _session->KickPlayer();
        return "Kick";
        break;
    case WARDEN_ACTION_BAN:
        {
            std::stringstream duration;
            duration << sWorld->getIntConfig(CONFIG_WARDEN_CLIENT_BAN_DURATION) << "s";
            std::string accountName;
            AccountMgr::GetName(_session->GetAccountId(), accountName);
            sWorld->BanAccount(BAN_ACCOUNT, accountName, ((longBan && false /*ZOMG!*/) ? "1209600s" : duration.str()), banReason, "Server");

            return "Ban";
        }
    default:
        break;
    }
    return "Undefined";
}

void WorldSession::HandleWardenDataOpcode(WorldPacket& recvData)
{
    if (!_warden || recvData.empty())
        return;

    _warden->DecryptData(recvData.contents(), recvData.size());
    uint8 opcode;
    recvData >> opcode;
    ;//sLog->outDebug(LOG_FILTER_WARDEN, "Got packet, opcode %02X, size %u", opcode, uint32(recvData.size()));
    recvData.hexlike();

    switch (opcode)
    {
        case WARDEN_CMSG_MODULE_MISSING:
            _warden->SendModuleToClient();
            break;
        case WARDEN_CMSG_MODULE_OK:
            _warden->RequestHash();
            break;
        case WARDEN_CMSG_CHEAT_CHECKS_RESULT:
            _warden->HandleData(recvData);
            break;
        case WARDEN_CMSG_MEM_CHECKS_RESULT:
            ;//sLog->outDebug(LOG_FILTER_WARDEN, "NYI WARDEN_CMSG_MEM_CHECKS_RESULT received!");
            break;
        case WARDEN_CMSG_HASH_RESULT:
            _warden->HandleHashResult(recvData);
            _warden->InitializeModule();
            break;
        case WARDEN_CMSG_MODULE_FAILED:
            ;//sLog->outDebug(LOG_FILTER_WARDEN, "NYI WARDEN_CMSG_MODULE_FAILED received!");
            break;
        default:
            ;//sLog->outDebug(LOG_FILTER_WARDEN, "Got unknown warden opcode %02X of size %u.", opcode, uint32(recvData.size() - 1));
            break;
    }
}
