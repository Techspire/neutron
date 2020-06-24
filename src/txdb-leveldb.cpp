// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2015-2020 The Neutron Developers
//
// Distributed under the MIT/X11 software license, see the accompanying
// file license.txt or http://www.opensource.org/licenses/mit-license.php.

#include <map>

#include <boost/version.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

#include <leveldb/env.h>
#include <leveldb/cache.h>
#include <leveldb/filter_policy.h>
#include <memenv/memenv.h>

#include "backtrace.h"
#include "blockindex.h"
#include "kernel.h"
#include "checkpoints.h"
#include "txdb.h"
#include "util.h"
#include "utiltime.h"
#include "main.h"

using namespace std;
using namespace boost;

extern std::atomic<bool> fRequestShutdown;

leveldb::DB *txdb; // global pointer for LevelDB object instance

static leveldb::Options GetOptions() {
    leveldb::Options options;
    int nCacheSizeMB = GetArg("-dbcache", 25);

    options.block_cache = leveldb::NewLRUCache(nCacheSizeMB * 1048576);
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);

    return options;
}

void init_blockindex(leveldb::Options& options, bool fRemoveOld = false) {
    // First time init.
    filesystem::path directory = GetDataDir() / "txleveldb";

    if (fRemoveOld)
    {
        filesystem::remove_all(directory); // remove directory
        unsigned int nFile = 1;

        while (true)
        {
            filesystem::path strBlockFile = GetDataDir() / strprintf("blk%04u.dat", nFile);

            // Break if no such file
            if (!filesystem::exists( strBlockFile))
                break;

            filesystem::remove(strBlockFile);

            nFile++;
        }
    }

    filesystem::create_directory(directory);
    LogPrintf("Opening LevelDB in %s\n", directory.string().c_str());
    leveldb::Status status = leveldb::DB::Open(options, directory.string(), &txdb);

    if (!status.ok())
    {
        throw runtime_error(strprintf("%s : error opening database environment %s",
                            __func__, status.ToString().c_str()));
    }
}

// CDB subclasses are created and destroyed VERY OFTEN. That's why
// we shouldn't treat this as a free operations.

CTxDB::CTxDB(const char* pszMode)
{
    assert(pszMode);
    activeBatch = NULL;
    fReadOnly = (!strchr(pszMode, '+') && !strchr(pszMode, 'w'));

    if (txdb)
    {
        pdb = txdb;
        return;
    }

    bool fCreate = strchr(pszMode, 'c');

    options = GetOptions();
    options.create_if_missing = fCreate;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);

    init_blockindex(options); // Init directory
    pdb = txdb;

    if (Exists(string("version")))
    {
        ReadVersion(nVersion);
        LogPrintf("%s : transaction index version is %d\n", __func__, nVersion);

        if (nVersion < DATABASE_VERSION)
        {
            LogPrintf("%s : required index version is %d, removing old database\n", DATABASE_VERSION);

            delete txdb;
            txdb = pdb = NULL;
            delete activeBatch;
            activeBatch = NULL;

            init_blockindex(options, true); // Remove directory and create new database
            pdb = txdb;

            bool fTmp = fReadOnly;
            fReadOnly = false;
            WriteVersion(DATABASE_VERSION); // Save transaction index version
            fReadOnly = fTmp;
        }
    }
    else if (fCreate)
    {
        bool fTmp = fReadOnly;
        fReadOnly = false;
        WriteVersion(DATABASE_VERSION);
        fReadOnly = fTmp;
    }

    LogPrintf("%s : opened leveldb successfully\n", __func__);
}

void CTxDB::Close()
{
    // Free these, otherwise we get memory leaks on shutdown
    for (auto i : mapBlockIndex)
        delete i.second;

    delete txdb;
    txdb = pdb = NULL;

    delete options.filter_policy;
    options.filter_policy = NULL;

    delete options.block_cache;
    options.block_cache = NULL;

    delete activeBatch;
    activeBatch = NULL;
}

bool CTxDB::TxnBegin()
{
    assert(!activeBatch);
    activeBatch = new leveldb::WriteBatch();
    return true;
}

bool CTxDB::TxnCommit()
{
    assert(activeBatch);
    leveldb::Status status = pdb->Write(leveldb::WriteOptions(), activeBatch);
    delete activeBatch;

    activeBatch = NULL;

    if (!status.ok())
    {
        LogPrintf("%s : leveldb batch commit failure: %s\n", __func__, status.ToString().c_str());
        return false;
    }

    return true;
}

class CBatchScanner : public leveldb::WriteBatch::Handler
{
public:
    std::string needle;
    bool *deleted;
    std::string *foundValue;
    bool foundEntry;

    CBatchScanner() : foundEntry(false) {}

    virtual void Put(const leveldb::Slice& key, const leveldb::Slice& value)
    {
        if (key.ToString() == needle)
        {
            foundEntry = true;
            *deleted = false;
            *foundValue = value.ToString();
        }
    }

    virtual void Delete(const leveldb::Slice& key)
    {
        if (key.ToString() == needle)
        {
            foundEntry = true;
            *deleted = true;
        }
    }
};

// When performing a read, if we have an active batch we need to check it first
// before reading from the database, as the rest of the code assumes that once
// a database transaction begins reads are consistent with it. It would be good
// to change that assumption in future and avoid the performance hit, though in
// practice it does not appear to be large.

bool CTxDB::ScanBatch(const CDataStream &key, string *value, bool *deleted) const
{
    assert(activeBatch);
    *deleted = false;
    CBatchScanner scanner;
    scanner.needle = key.str();
    scanner.deleted = deleted;
    scanner.foundValue = value;
    leveldb::Status status = activeBatch->Iterate(&scanner);

    if (!status.ok())
        throw runtime_error(status.ToString());

    return scanner.foundEntry;
}

bool CTxDB::ReadTxIndex(uint256 hash, CTxIndex& txindex)
{
    assert(!fClient);
    txindex.SetNull();

    return Read(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::UpdateTxIndex(uint256 hash, const CTxIndex& txindex)
{
    assert(!fClient);
    return Write(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::AddTxIndex(const CTransaction& tx, const CDiskTxPos& pos, int nHeight)
{
    assert(!fClient);

    // Add to tx index
    uint256 hash = tx.GetHash();
    CTxIndex txindex(pos, tx.vout.size());

    return Write(make_pair(string("tx"), hash), txindex);
}

bool CTxDB::EraseTxIndex(const CTransaction& tx)
{
    assert(!fClient);
    uint256 hash = tx.GetHash();

    return Erase(make_pair(string("tx"), hash));
}

bool CTxDB::ContainsTx(uint256 hash)
{
    assert(!fClient);
    return Exists(make_pair(string("tx"), hash));
}

bool CTxDB::ReadDiskTx(uint256 hash, CTransaction& tx, CTxIndex& txindex)
{
    assert(!fClient);
    tx.SetNull();

    if (!ReadTxIndex(hash, txindex))
        return false;

    return (tx.ReadFromDisk(txindex.pos));
}

bool CTxDB::ReadDiskTx(uint256 hash, CTransaction& tx)
{
    CTxIndex txindex;
    return ReadDiskTx(hash, tx, txindex);
}

bool CTxDB::ReadDiskTx(COutPoint outpoint, CTransaction& tx, CTxIndex& txindex)
{
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool CTxDB::ReadDiskTx(COutPoint outpoint, CTransaction& tx)
{
    CTxIndex txindex;
    return ReadDiskTx(outpoint.hash, tx, txindex);
}

bool CTxDB::ContainsBlockIndex(const uint256& hash)
{
    return Exists(make_pair(string("blockindex"), hash));
}

bool CTxDB::ReadBlockIndex(const uint256& hash, CDiskBlockIndex& blockindex)
{
    return Read(make_pair(string("blockindex"), hash), blockindex);
}

bool CTxDB::WriteBlockIndex(const CDiskBlockIndex& blockindex)
{
    return Write(make_pair(string("blockindex"), blockindex.GetBlockHash()), blockindex);
}

bool CTxDB::ReadHashBestChain(uint256& hashBestChain)
{
    return Read(string("hashBestChain"), hashBestChain);
}

bool CTxDB::WriteHashBestChain(uint256 hashBestChain)
{
    return Write(string("hashBestChain"), hashBestChain);
}

bool CTxDB::ReadBestInvalidTrust(CBigNum& bnBestInvalidTrust)
{
    return Read(string("bnBestInvalidTrust"), bnBestInvalidTrust);
}

bool CTxDB::WriteBestInvalidTrust(CBigNum bnBestInvalidTrust)
{
    return Write(string("bnBestInvalidTrust"), bnBestInvalidTrust);
}

bool CTxDB::ReadSyncCheckpoint(uint256& hashCheckpoint)
{
    return Read(string("hashSyncCheckpoint"), hashCheckpoint);
}

bool CTxDB::WriteSyncCheckpoint(uint256 hashCheckpoint)
{
    return Write(string("hashSyncCheckpoint"), hashCheckpoint);
}

bool CTxDB::ReadCheckpointPubKey(string& strPubKey)
{
    return Read(string("strCheckpointPubKey"), strPubKey);
}

bool CTxDB::WriteCheckpointPubKey(const string& strPubKey)
{
    return Write(string("strCheckpointPubKey"), strPubKey);
}

static CBlockIndex *InsertBlockIndex(uint256 hash)
{
    if (hash == 0)
        return NULL;

    // Return existing
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);

    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();

    if (!pindexNew)
        throw runtime_error(strprintf("%s : new CBlockIndex failed", __func__));

    mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    return pindexNew;
}

bool CTxDB::LoadBlockIndex()
{
    if (mapBlockIndex.size() > 0)
    {
        // Already loaded once in this session. It can happen during migration from BDB
        return true;
    }

    // out of the DB and into mapBlockIndex.
    leveldb::Iterator *iterator = pdb->NewIterator(leveldb::ReadOptions());

    // Seek to start key.
    CDataStream ssStartKey(SER_DISK, CLIENT_VERSION);
    ssStartKey << make_pair(string("blockindex"), uint256(0));
    iterator->Seek(ssStartKey.str());

    // Now read each entry.
    while (iterator->Valid())
    {
        // Unpack keys and values.
        CDataStream ssKey(SER_DISK, CLIENT_VERSION);
        ssKey.write(iterator->key().data(), iterator->key().size());
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.write(iterator->value().data(), iterator->value().size());
        string strType;
        ssKey >> strType;

        // Did we reach the end of the data to read?
        if (fRequestShutdown || strType != "blockindex")
            break;

        CDiskBlockIndex diskindex;
        ssValue >> diskindex;
        uint256 blockHash = diskindex.GetBlockHash();

        // Construct block index object
        CBlockIndex* pindexNew    = InsertBlockIndex(blockHash);
        pindexNew->pprev          = InsertBlockIndex(diskindex.hashPrev);
        pindexNew->pnext          = InsertBlockIndex(diskindex.hashNext);
        pindexNew->nFile          = diskindex.nFile;
        pindexNew->nBlockPos      = diskindex.nBlockPos;
        pindexNew->nHeight        = diskindex.nHeight;
        pindexNew->nMint          = diskindex.nMint;
        pindexNew->nMoneySupply   = diskindex.nMoneySupply;
        pindexNew->nFlags         = diskindex.nFlags;
        pindexNew->nStakeModifier = diskindex.nStakeModifier;
        pindexNew->prevoutStake   = diskindex.prevoutStake;
        pindexNew->nStakeTime     = diskindex.nStakeTime;
        pindexNew->hashProof      = diskindex.hashProof;
        pindexNew->nVersion       = diskindex.nVersion;
        pindexNew->hashMerkleRoot = diskindex.hashMerkleRoot;
        pindexNew->nTime          = diskindex.nTime;
        pindexNew->nBits          = diskindex.nBits;
        pindexNew->nNonce         = diskindex.nNonce;

        // Watch for genesis block
        // if (pindexGenesisBlock == NULL && blockHash == (!fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet))
        //     pindexGenesisBlock = pindexNew;

        if (!pindexNew->CheckIndex())
        {
            delete iterator;
            return error("%s : CheckIndex failed at %d", __func__, pindexNew->nHeight);
        }

        if (pindexNew->IsProofOfStake())
            setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));

        iterator->Next();
    }

    delete iterator;

    if (fRequestShutdown)
        return true;

    // Calculate nChainTrust
    vector<pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());

    BOOST_FOREACH(const PAIRTYPE(uint256, CBlockIndex*)& item, mapBlockIndex)
    {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
    }

    sort(vSortedByHeight.begin(), vSortedByHeight.end());

    BOOST_FOREACH(const PAIRTYPE(int, CBlockIndex*)& item, vSortedByHeight)
    {
        CBlockIndex* pindex = item.second;
        pindex->nChainTrust = (pindex->pprev ? pindex->pprev->nChainTrust : 0) + pindex->GetBlockTrust();
        pindex->nStakeModifierChecksum = GetStakeModifierChecksum(pindex);

        if (!CheckStakeModifierCheckpoints(pindex->nHeight, pindex->nStakeModifierChecksum))
        {
            return error("%s : failed stake modifier checkpoint height=%d, modifier=0x%016" PRIx64,
                         __func__, pindex->nHeight, pindex->nStakeModifier);
        }
    }

    // Load hashBestChain pointer to end of best chain
    if (!ReadHashBestChain(hashBestChain))
    {
        if (!blockIndex.contains(fTestNet ? hashGenesisBlock : hashGenesisBlockTestNet))
            return true;

        return error("%s : hashBestChain not loaded", __func__);
    }

    if (!mapBlockIndex.count(hashBestChain))
        return error("%s : hashBestChain not found in the block index", __func__);

    pindexBest = mapBlockIndex[hashBestChain];
    nBestHeight = pindexBest->nHeight;
    nBestChainTrust = pindexBest->nChainTrust;

    LogPrintf("%s : hashBestChain=%s height=%d trust=%s date=%s\n", __func__,
              hashBestChain.ToString().substr(0,20).c_str(),
              nBestHeight, CBigNum(nBestChainTrust).ToString().c_str(),
              DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()).c_str());

    if (!ReadSyncCheckpoint(Checkpoints::hashSyncCheckpoint))
        return error("%s : hashSyncCheckpoint not loaded", __func__);

    LogPrintf("%s : synchronized checkpoint %s\n", __func__,
              Checkpoints::hashSyncCheckpoint.ToString().c_str());

    // Load bnBestInvalidTrust, OK if it doesn't exist
    CBigNum bnBestInvalidTrust;
    ReadBestInvalidTrust(bnBestInvalidTrust);
    nBestInvalidTrust = bnBestInvalidTrust.getuint256();

    // Verify blocks in the best chain
    int nCheckLevel = GetArg("-checklevel", 1);
    int nCheckDepth = GetArg( "-checkblocks", 500);

    if (nCheckDepth == 0)
        nCheckDepth = 1000000000; // suffices until the year 19000

    if (nCheckDepth > nBestHeight)
        nCheckDepth = nBestHeight;

    LogPrintf("%s : verifying last %i blocks at level %i\n", __func__, nCheckDepth, nCheckLevel);

    CBlockIndex* pindexFork = NULL;
    map<pair<unsigned int, unsigned int>, CBlockIndex*> mapBlockPos;

    for (CBlockIndex* pindex = pindexBest; pindex && pindex->pprev; pindex = pindex->pprev)
    {
        if (fRequestShutdown || pindex->nHeight < nBestHeight-nCheckDepth)
            break;

        CBlock block;

        if (!block.ReadFromDisk(pindex))
            return error("%s : block.ReadFromDisk failed", __func__);

        // check level 1: verify block validity
        // check level 7: verify block signature too
        if (nCheckLevel>0 && !block.CheckBlock(true, true, (nCheckLevel>6)))
        {
            LogPrintf("%s : [WARNING] found bad block at %d, hash=%s\n", __func__,
                      pindex->nHeight, pindex->GetBlockHash().ToString().c_str());

            pindexFork = pindex->pprev;
        }

        // check level 2: verify transaction index validity
        if (nCheckLevel > 1)
        {
            pair<unsigned int, unsigned int> pos = make_pair(pindex->nFile, pindex->nBlockPos);
            mapBlockPos[pos] = pindex;

            BOOST_FOREACH(const CTransaction &tx, block.vtx)
            {
                uint256 hashTx = tx.GetHash();
                CTxIndex txindex;

                if (ReadTxIndex(hashTx, txindex))
                {
                    // check level 3: checker transaction hashes
                    if (nCheckLevel > 2 || pindex->nFile != txindex.pos.nFile || pindex->nBlockPos != txindex.pos.nBlockPos)
                    {
                        // either an error or a duplicate transaction
                        CTransaction txFound;

                        if (!txFound.ReadFromDisk(txindex.pos))
                        {
                            LogPrintf("%s : [WARNING] cannot read mislocated transaction %s\n",
                                      __func__, hashTx.ToString().c_str());

                            pindexFork = pindex->pprev;
                        }
                        else if (txFound.GetHash() != hashTx) // not a duplicate tx
                        {
                            LogPrintf("%s : [WARNING] invalid tx position for %s\n",
                                      __func__, hashTx.ToString().c_str());

                            pindexFork = pindex->pprev;
                        }
                    }

                    // check level 4: check whether spent txouts were spent within the main chain
                    unsigned int nOutput = 0;

                    if (nCheckLevel > 3)
                    {
                        BOOST_FOREACH(const CDiskTxPos &txpos, txindex.vSpent)
                        {
                            if (!txpos.IsNull())
                            {
                                pair<unsigned int, unsigned int> posFind = make_pair(txpos.nFile, txpos.nBlockPos);

                                if (!mapBlockPos.count(posFind))
                                {
                                    LogPrintf("%s : [WARNING] found bad spend at %d, hashBlock=%s, hashTx=%s\n",
                                              __func__, pindex->nHeight, pindex->GetBlockHash().ToString().c_str(),
                                              hashTx.ToString().c_str());

                                    pindexFork = pindex->pprev;
                                }

                                // check level 6: check whether spent txouts were spent by a valid transaction that consume them
                                if (nCheckLevel > 5)
                                {
                                    CTransaction txSpend;
                                    if (!txSpend.ReadFromDisk(txpos))
                                    {
                                        LogPrintf("%s : [WARNING] cannot read spending transaction of %s:%i from disk\n",
                                                  __func__, hashTx.ToString().c_str(), nOutput);

                                        pindexFork = pindex->pprev;
                                    }
                                    else if (!txSpend.CheckTransaction())
                                    {
                                        LogPrintf("%s : [WARNING] spending transaction of %s:%i is invalid\n",
                                                  __func__, hashTx.ToString().c_str(), nOutput);

                                        pindexFork = pindex->pprev;
                                    }
                                    else
                                    {
                                        bool fFound = false;

                                        BOOST_FOREACH(const CTxIn &txin, txSpend.vin)
                                        {
                                            if (txin.prevout.hash == hashTx && txin.prevout.n == nOutput)
                                                fFound = true;
                                        }

                                        if (!fFound)
                                        {
                                            LogPrintf("%s : [WARNING] spending transaction of %s:%i does not spend it\n",
                                                      __func__, hashTx.ToString().c_str(), nOutput);

                                            pindexFork = pindex->pprev;
                                        }
                                    }
                                }
                            }

                            nOutput++;
                        }
                    }
                }

                // check level 5: check whether all prevouts are marked spent
                if (nCheckLevel > 4)
                {
                     BOOST_FOREACH(const CTxIn &txin, tx.vin)
                     {
                          CTxIndex txindex;

                          if (ReadTxIndex(txin.prevout.hash, txindex))
                          {
                              if (txindex.vSpent.size() - 1 < txin.prevout.n || txindex.vSpent[txin.prevout.n].IsNull())
                              {
                                  LogPrintf("%s : [WARNING] found unspent prevout %s:%i in %s\n",
                                            __func__, txin.prevout.hash.ToString().c_str(), txin.prevout.n,
                                            hashTx.ToString().c_str());

                                  pindexFork = pindex->pprev;
                              }
                          }
                     }
                }
            }
        }
    }

    if (pindexFork && !fRequestShutdown)
    {
        // Reorg back to the fork
        LogPrintf("%s : [WARNING] moving best chain pointer back to block %d\n",
                  __func__, pindexFork->nHeight);

        CBlock block;

        if (!block.ReadFromDisk(pindexFork))
            return error("%s : block.ReadFromDisk failed", __func__);

        CTxDB txdb;
        block.SetBestChain(txdb, pindexFork);
    }

    return true;
}
