// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2015 The Hivemind Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <map>
#include <vector>

#include "miner.h"

#include "amount.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "hash.h"
#include "main.h"
#include "net.h"
#include "pow.h"
#include "timedata.h"
#include "txdb.h"
#include "util.h"
#include "utilmoneystr.h"
#ifdef ENABLE_WALLET
#include "wallet.h"
#endif

#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>

using namespace std;

extern CMarketTreeDB *pmarkettree;

//////////////////////////////////////////////////////////////////////////////
//
// HivemindMiner
//

// getOutcomeTx
// for a given branch...
// calc_rep_tx:
//    for a given tau, the miner calcs reputation transactions at
//        height n*tau + ballotTime + unsealTime
//    it considers all decisions in that branch ending in
//        heights ((n-1)*tau, n*tau]
//    it considers all sealed votes submitted for those decisions in
//        heights (n*tau, n*tau+ballotTime]
//    it considers all revealed votes submitted for those decisions in
//        heights (n*tau+ballotTime, n*tau+ballotTime+unsealTime]
// calc_coin_tx:
//    any market with max { maturation, decisionID[].eventOverBy}
//        at this height,
//      1) all trade payouts are calculated, and
//      2) the market author fee is calculated.
//
void getBranchOutcome(CMutableTransaction &tx, marketBranch *branch, uint32_t height)
{
    /* Check that branch actually points to something */
    if (!branch) return;

    /* Make sure the current height is a factor of tau */
    if (height % branch->tau) return;

    /* Retrieve the votes for this height. */
    vector<marketRevealVote *> revealvotes = pmarkettree->GetRevealVotes(branch->GetHash(), height);
    /* Create a map of new votes, indexed by their keyID(s) */
    map<CKeyID, const marketRevealVote *> voteMap;
    for(size_t i=0; i < revealvotes.size(); i++)
       voteMap[ revealvotes[i]->keyID ] = revealvotes[i];

    /* outcome for this height */
    struct marketOutcome *outcome = NULL;

    /* create new outcome and populate */
    outcome = new marketOutcome;
    outcome->branchid = branch->GetHash();
    outcome->nDecisions = 0;
    outcome->NA = 2016; /* if conflicts, to be changed (TODO) */
    outcome->alpha = branch->alpha;
    outcome->tol = branch->tol;
    outcome->nVoters = 0;

    // TODO adjust
    /* The range of decision ending times to be included in the new outcome */
    uint32_t maxHeight = height; // Max = the current height, which is a multiple of tau.
    uint32_t minHeight = maxHeight - (branch->tau - 1); // Min = the begining of this tau period

    /* Get the list of decisions on the branch, and create decisionMap */
    vector<marketDecision *> decisions = pmarkettree->GetDecisions(branch->GetHash());
    map<uint256, const marketDecision *> decisionMap;
    for(size_t i=0; i < decisions.size(); i++)
        decisionMap[ decisions[i]->GetHash() ] = decisions[i];

    /* Find the decisions within our range */
    for(size_t i=0; i < decisions.size(); i++) {
        const marketDecision *decision = decisions[i];
        if (decision->eventOverBy <= minHeight)
            continue;
        if (decision->eventOverBy > maxHeight)
            continue;

        /* Add relevant decisions to the new outcome */
        outcome->nDecisions++;
        outcome->decisionIDs.push_back(decision->GetHash());
        outcome->isScaled.push_back(decision->isScaled);
    }

    /* Get the list of previous outcomes on this branch */
    vector<marketOutcome *> outcomes = pmarkettree->GetOutcomes(branch->GetHash());

    /* Try to find the previous outcome transaction */
    CTransaction previousOutcomeTx;
    uint32_t previousOutcomeHeight = 0;
    for(size_t i = 0; i < outcomes.size(); ++i) {
        if (outcomes[i]->nHeight < previousOutcomeHeight)
            continue;
        if (outcomes[i]->tx.vout.empty())
            continue;
        previousOutcomeHeight = outcomes[i]->nHeight;
        previousOutcomeTx = outcomes[i]->tx;
    }

    /* Populate the vote matrix [nVoters][nDecisions] */
    if (outcome->nDecisions) {
        /* Retrieve the votes for this height. */
        vector<marketRevealVote *> revealvotes = pmarkettree->GetRevealVotes(branch->GetHash(), height);

        /* Create a map of new votes, indexed by their owner's keyID */
        map<CKeyID, const marketRevealVote *> voteMap;
        for(size_t i=0; i < revealvotes.size(); i++)
           voteMap[ revealvotes[i]->keyID ] = revealvotes[i];

        // Clear and resize the vote matrix for new outcome calculation
        outcome->voteMatrix.clear();
        outcome->voteMatrix.resize(voteMap.size()*outcome->nDecisions, outcome->NA);

        /* Go through the reveal votes and collect voter keys */
        for(uint32_t i=0; i < revealvotes.size(); i++) {
            outcome->voterIDs.push_back(revealvotes.at(i)->keyID);

            uint256 txid = revealvotes.at(i)->txid;
//            pcoinsTip->GetCoins()
// TODO OLD REP
            outcome->oldRep.push_back(25000);
            map<CKeyID, const marketRevealVote *>::const_iterator vit
                = voteMap.find(revealvotes.at(i)->keyID);
            if (vit != voteMap.end()) {
                const marketRevealVote *vote = vit->second;
                const vector<uint256> &decisionIDs = vote->decisionIDs;
                const vector<uint64_t> &decisionVotes = vote->decisionVotes;
                if (decisionIDs.size() == decisionVotes.size()) {
                    for(uint32_t j=0; j < decisionIDs.size(); j++) {
                        /* find this voter's response on that decision */
                        uint64_t resp = outcome->NA;
                        for(uint32_t k=0; k < decisionIDs.size(); k++)
                            if (decisionIDs[k] == outcome->decisionIDs[j])
                                { resp = decisionVotes[k]; break; }
                        outcome->voteMatrix[ i*outcome->nDecisions + j] = resp;
                    }
                }
            }
            outcome->nVoters++;
        }
        /* clean up */
        for(size_t i=0; i < revealvotes.size(); i++)
            delete revealvotes[i];
    }

    /* Calculate new outcome, add reputation payouts to transaction */
    if (outcome->nVoters) {
        int ret = outcome->calc();

        if (ret == 0) {
            for(uint32_t i=0; i < outcome->voterIDs.size(); i++) {
                // Get voter information
                CKeyID keyID = outcome->voterIDs[i];
                CScript script;
                script << OP_DUP << OP_HASH160 << ToByteVector(keyID) << OP_EQUALVERIFY << OP_CHECKSIG;

                // Pay voter
                tx.vout.push_back(CTxOut(1e-8*outcome->smoothedRep[i], script));
            }
        }
    }

    /* Find all of the markets on this branch */
    vector<marketMarket *> markets;
    for (unsigned int x = 0; x < decisions.size(); x++) {
        // Get the markets for this decision
        uint256 uDecision(decisions.at(x)->GetHash());
        vector<marketMarket* > decisionMarkets = pmarkettree->GetMarkets(uDecision);

        // Add the markets to the markets vector
        for (unsigned int y = 0; y < decisionMarkets.size(); y++) {
            markets.push_back(decisionMarkets.at(y));
        }
    }

    /* find which markets have just ended */
    uint32_t nMarketsEnded = 0; // Count of markets that have ended
    uint8_t *marketNeedsCalc = NULL; // Array indicating markets that need calculation (1 = calc 0 = skip)
    if (!markets.empty()) {
        marketNeedsCalc = new uint8_t [markets.size()];
        memset(marketNeedsCalc, 0, markets.size());
        for(size_t i=0; i < markets.size(); i++) {
            const marketMarket *market = markets[i];
            if (market->maturation <= minHeight)
                continue;
            if (market->maturation > maxHeight)
                continue;

            marketNeedsCalc[i] = 1;
            nMarketsEnded++;
        }
    }

    /* If markets have ended, calculate the payouts */
    if (nMarketsEnded) {
        /* Loop through the list of markets, and calculate those that need to be */
        for(uint32_t i=0; i < markets.size(); i++) {
            if (!marketNeedsCalc[i])
                continue;
            /* populate the market's decisionsFinal and isScaled arrays */
            vector<uint64_t> decisionsFinals;
            vector<uint64_t> isScaleds;
            vector<uint64_t> mins;
            vector<uint64_t> maxs;
            for(size_t j=0; j < markets[i]->decisionIDs.size(); j++) {
                map<uint256,const marketDecision *>::const_iterator it
                        = decisionMap.find(markets[i]->decisionIDs[j]);
                uint64_t decisionFinal = 2016;
                uint64_t min = 0;
                uint64_t max = 0;
                uint8_t isScaled = 0;
                if (it != decisionMap.end()) {
                    min = it->second->min;
                    max = it->second->max;
                    /* get the outcome for this decision */
                    const marketOutcome *decisionOutcome = NULL;
                    if (it->second->eventOverBy == height && outcome) {
                        decisionOutcome = outcome;
                    }

                    /* find the decisionFinal in this outcome */
                    if (decisionOutcome) {
                        for(size_t k=0; k < outcome->decisionIDs.size(); i++) {
                            if (decisionOutcome->decisionIDs[k] != markets[i]->decisionIDs[j])
                                continue;
                            decisionFinal = decisionOutcome->decisionsFinal[k];
                            isScaled = decisionOutcome->isScaled[k];
                            break;
                        }
                    }
                }
                decisionsFinals.push_back(decisionFinal);
                isScaleds.push_back(isScaled);
                mins.push_back(min);
                maxs.push_back(max);
            }

            /* the trades */
            vector<marketTrade *> trades = pmarkettree->GetTrades(markets[i]->GetHash());
            for(uint32_t j=0; j < trades.size(); j++) {
                if (!trades[j]->isBuy)
                    continue;
                /* TODO: lookup and skip if previously sold */
                uint64_t nShares = trades[j]->nShares;
                uint32_t decisionState = trades[j]->decisionState;
                /* iterate through all decision finals */
                double payout = 1.0;
                for(uint32_t k=0; k < decisionsFinals.size(); k++) {
                    uint8_t state = (decisionState >> k) & 1;
                    if (isScaleds[k] && (maxs[k] > mins[k])) {
                        if (state == 0) {
                            payout *= (maxs[k] - decisionsFinals[k]) / (maxs[k] - mins[k]);
                        } else if (state == 1) {
                            payout *= (decisionsFinals[k] - mins[k]) / (maxs[k] - mins[k]);
                        }
                    } else if ((state == 0) && (decisionsFinals[k] > 0.5*1e8)) {
                         payout = 0;
                         break;
                    } else if ((state == 1) && (decisionsFinals[k] < 0.5*1e8)) {
                        payout = 0;
                        break;
                    }
                }

                /* If there is payout to be made, create the script and add it to the coinTX */
                if (payout > 0.0) {
//                    CScript script;
//                    script << OP_DUP << OP_HASH160 << ToByteVector(trades[j]->keyID) << OP_EQUALVERIFY << OP_CHECKSIG;
//                    mtx.vout.push_back(CTxOut(nShares*payout, script));
                }

                /* Cleanup */
                delete trades[j];
            }
        }
    }

    // add outcome
    if (outcome) {
        tx.vout.push_back(CTxOut(0, outcome->GetScript()));
    }

    /* clean up */
    for(size_t i=0; i < markets.size(); i++)
        delete markets[i];
    if (marketNeedsCalc)
        delete [] marketNeedsCalc;

    for(size_t i=0; i < decisions.size(); i++)
        delete decisions[i];
    for(size_t i=0; i < outcomes.size(); i++)
        delete outcomes[i];
    if (outcome)
        delete outcome;
}

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.
// The COrphan class keeps track of these 'temporary orphans' while
// CreateBlock is figuring out which transactions to include.
//
class COrphan
{
public:
    const CTransaction* ptx;
    set<uint256> setDependsOn;
    CFeeRate feeRate;
    double dPriority;

    COrphan(const CTransaction* ptxIn) : ptx(ptxIn), feeRate(0), dPriority(0)
    {
    }
};

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;

// We want to sort transactions by priority and fee rate, so:
typedef boost::tuple<double, CFeeRate, const CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;

public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) { }

    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee)
        {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        }
        else
        {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};

void UpdateTime(CBlockHeader* pblock, const CBlockIndex* pindexPrev)
{
    pblock->nTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    // Updating time can change work required on testnet:
    if (Params().AllowMinDifficultyBlocks())
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock);
}

CBlockTemplate* CreateNewBlock(const CScript& scriptPubKeyIn)
{
    // Create new block
    auto_ptr<CBlockTemplate> pblocktemplate(new CBlockTemplate());
    if(!pblocktemplate.get())
        return NULL;
    CBlock *pblock = &pblocktemplate->block; // pointer for convenience

    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (Params().MineBlocksOnDemand())
        pblock->nVersion = GetArg("-blockversion", pblock->nVersion);

    // Create coinbase tx
    CMutableTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);
    txNew.vout[0].scriptPubKey = scriptPubKeyIn;

    // Add dummy coinbase tx as first transaction
    pblock->vtx.push_back(CTransaction());
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    // Add branch outcome output(s)
    uint32_t height = chainActive.Height() + 1;
    vector<marketBranch *> branches = pmarkettree->GetBranches();
    for(size_t i=0; i < branches.size(); i++) {
        getBranchOutcome(txNew, branches[i], height);
    }
    /* clean up */
    for(size_t i=0; i < branches.size(); i++)
        delete branches[i];

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", DEFAULT_BLOCK_MIN_SIZE);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Collect memory pool transactions into the block
    CAmount nFees = 0;

    {
        LOCK2(cs_main, mempool.cs);
        CBlockIndex* pindexPrev = chainActive.Tip();
        const int nHeight = pindexPrev->nHeight + 1;
        CCoinsViewCache view(pcoinsTip);

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;
        bool fPrintPriority = GetBoolArg("-printpriority", false);

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for (map<uint256, CTxMemPoolEntry>::iterator mi = mempool.mapTx.begin();
             mi != mempool.mapTx.end(); ++mi)
        {
            const CTransaction& tx = mi->second.GetTx();
            if (tx.IsCoinBase() || !IsFinalTx(tx, nHeight))
                continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            CAmount nTotalIn = 0;
            bool fMissingInputs = false;
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {
                // Read prev transaction
                if (!view.HaveCoins(txin.prevout.hash))
                {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash))
                    {
                        LogPrintf("ERROR: mempool transaction missing input\n");
                        if (fDebug) assert("mempool transaction missing input" == 0);
                        fMissingInputs = true;
                        if (porphan)
                            vOrphan.pop_back();
                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }
                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].GetTx().vout[txin.prevout.n].nValue;
                    continue;
                }
                const CCoins* coins = view.AccessCoins(txin.prevout.hash);
                assert(coins);

                CAmount nValueIn = coins->vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = nHeight - coins->nHeight;

                dPriority += (double)nValueIn * nConf;
            }
            if (fMissingInputs) continue;

            // Priority is sum(valuein * age) / modified_txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority = tx.ComputePriority(dPriority, nTxSize);

            uint256 hash = tx.GetHash();
            mempool.ApplyDeltas(hash, dPriority, nTotalIn);

            CFeeRate feeRate(nTotalIn-tx.GetValueOut(), nTxSize);

            if (porphan)
            {
                porphan->dPriority = dPriority;
                porphan->feeRate = feeRate;
            }
            else
                vecPriority.push_back(TxPriority(dPriority, feeRate, &mi->second.GetTx()));
        }

        // Collect transactions into block
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty())
        {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            CFeeRate feeRate = vecPriority.front().get<1>();
            const CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = GetLegacySigOpCount(tx);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            // Skip free transactions if we're past the minimum block size:
            const uint256& hash = tx.GetHash();
            double dPriorityDelta = 0;
            CAmount nFeeDelta = 0;
            mempool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);
            if (fSortedByFee && (dPriorityDelta <= 0) && (nFeeDelta <= 0) && (feeRate < ::minRelayTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                continue;

            // Prioritize by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || !AllowFree(dPriority)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            if (!view.HaveInputs(tx))
                continue;

            CAmount nTxFees = view.GetValueIn(tx)-tx.GetValueOut();

            nTxSigOps += GetP2SHSigOpCount(tx, view);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            // Note that flags: we don't want to set mempool/IsStandard()
            // policy here, but we still have to ensure that the block we
            // create only contains transactions that are valid in new blocks.
            CValidationState state;
            if (!CheckInputs(tx, state, view, true, MANDATORY_SCRIPT_VERIFY_FLAGS, true))
                continue;

            UpdateCoins(tx, state, view, nHeight);

            // Added
            pblock->vtx.push_back(tx);
            pblocktemplate->vTxFees.push_back(nTxFees);
            pblocktemplate->vTxSigOps.push_back(nTxSigOps);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fPrintPriority)
            {
                LogPrintf("priority %.1f fee %s txid %s\n",
                    dPriority, feeRate.ToString(), tx.GetHash().ToString());
            }

            // Add transactions that depend on this one to the priority queue
            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->feeRate, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;
        LogPrintf("CreateNewBlock(): total size %u\n", nBlockSize);

        // Compute final coinbase transaction.
        txNew.vout[0].nValue = GetBlockValue(nHeight, nFees) - 100000000;
        txNew.vin[0].scriptSig = CScript() << nHeight << OP_0;
        pblock->vtx[0] = txNew;
        pblocktemplate->vTxFees[0] = -nFees;

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        UpdateTime(pblock, pindexPrev);
        pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock);
        pblock->nNonce         = 0;
        pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(pblock->vtx[0]);

        CValidationState state;
        if (!TestBlockValidity(state, *pblock, pindexPrev, false, false)) {     
            throw std::runtime_error("CreateNewBlock() : TestBlockValidity failed");
        }
    }

    return pblocktemplate.release();
}

void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = txCoinbase;
    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}

#ifdef ENABLE_WALLET
//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//
double dHashesPerSec = 0.0;
int64_t nHPSTimerStart = 0;

//
// ScanHash scans nonces looking for a hash with at least some zero bits.
// The nonce is usually preserved between calls, but periodically or if the
// nonce is 0xffff0000 or above, the block is rebuilt and nNonce starts over at
// zero.
//
bool static ScanHash(const CBlockHeader *pblock, uint32_t& nNonce, uint256 *phash)
{
    // Write the first 76 bytes of the block header to a double-SHA256 state.
    CHash256 hasher;
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << *pblock;
    assert(ss.size() == 80);
    hasher.Write((unsigned char*)&ss[0], 76);

    while (true) {
        nNonce++;

        // Write the last 4 bytes of the block header (the nonce) to a copy of
        // the double-SHA256 state, and compute the result.
        CHash256(hasher).Write((unsigned char*)&nNonce, 4).Finalize((unsigned char*)phash);

        // Return the nonce if the hash has at least some zero bits,
        // caller will check if it has enough to reach the target
        if (((uint16_t*)phash)[15] == 0)
            return true;

        // If nothing found after trying for a while, return -1
        if ((nNonce & 0xffff) == 0)
            return false;
        if ((nNonce & 0xfff) == 0)
            boost::this_thread::interruption_point();
    }
}

CBlockTemplate* CreateNewBlockWithKey(CReserveKey& reservekey)
{
    CPubKey pubkey;
    if (!reservekey.GetReservedKey(pubkey))
        return NULL;

    CScript scriptPubKey = CScript() << ToByteVector(pubkey) << OP_CHECKSIG;
    return CreateNewBlock(scriptPubKey);
}

static bool ProcessBlockFound(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue));

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("HivemindMiner : generated block is stale");
    }

    // Remove key from key pool
    reservekey.KeepKey();

    // Track how many getdata requests this block gets
    {
        LOCK(wallet.cs_wallet);
        wallet.mapRequestCount[pblock->GetHash()] = 0;
    }

    // Process this block the same as if we had received it from another node
    CValidationState state;
    if (!ProcessNewBlock(state, NULL, pblock))
        return error("HivemindMiner : ProcessNewBlock, block not accepted");

    return true;
}

void static HivemindMiner(CWallet *pwallet)
{
    LogPrintf("HivemindMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("hivemind-miner");

    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);
    unsigned int nExtraNonce = 0;

    try {
        while (true) {
            if (Params().MiningRequiresPeers()) {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                while (vNodes.empty())
                    MilliSleep(1000);
            }

            //
            // Create new block
            //
            unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
            CBlockIndex* pindexPrev = chainActive.Tip();

            auto_ptr<CBlockTemplate> pblocktemplate(CreateNewBlockWithKey(reservekey));
            if (!pblocktemplate.get())
            {
                LogPrintf("Error in HivemindMiner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                return;
            }
            CBlock *pblock = &pblocktemplate->block;
            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

            LogPrintf("Running HivemindMiner with %u transactions in block (%u bytes)\n", pblock->vtx.size(),
                ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

            //
            // Search
            //
            int64_t nStart = GetTime();
            arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);
            uint256 hash;
            uint32_t nNonce = 0;
            uint32_t nOldNonce = 0;
            while (true) {
                bool fFound = ScanHash(pblock, nNonce, &hash);
                uint32_t nHashesDone = nNonce - nOldNonce;
                nOldNonce = nNonce;

                // Check if something found
                if (fFound)
                {
                    if (UintToArith256(hash) <= hashTarget)
                    {
                        // Found a solution
                        pblock->nNonce = nNonce;
                        assert(hash == pblock->GetHash());

                        SetThreadPriority(THREAD_PRIORITY_NORMAL);
                        LogPrintf("HivemindMiner:\n");
                        LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", hash.GetHex(), hashTarget.GetHex());
                        ProcessBlockFound(pblock, *pwallet, reservekey);
                        SetThreadPriority(THREAD_PRIORITY_LOWEST);

                        // In regression test mode, stop mining after a block is found.
                        if (Params().MineBlocksOnDemand())
                            throw boost::thread_interrupted();

                        break;
                    }
                }

                // Meter hashes/sec
                static int64_t nHashCounter;
                if (nHPSTimerStart == 0)
                {
                    nHPSTimerStart = GetTimeMillis();
                    nHashCounter = 0;
                }
                else
                    nHashCounter += nHashesDone;
                if (GetTimeMillis() - nHPSTimerStart > 4000)
                {
                    static CCriticalSection cs;
                    {
                        LOCK(cs);
                        if (GetTimeMillis() - nHPSTimerStart > 4000)
                        {
                            dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                            nHPSTimerStart = GetTimeMillis();
                            nHashCounter = 0;
                            static int64_t nLogTime;
                            if (GetTime() - nLogTime > 30 * 60)
                            {
                                nLogTime = GetTime();
                                LogPrintf("hashmeter %6.0f khash/s\n", dHashesPerSec/1000.0);
                            }
                        }
                    }
                }

                // Check for stop or if block needs to be rebuilt
                boost::this_thread::interruption_point();
                // Regtest mode doesn't require peers
                if (vNodes.empty() && Params().MiningRequiresPeers())
                    break;
                if (nNonce >= 0xffff0000)
                    break;
                if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                    break;
                if (pindexPrev != chainActive.Tip())
                    break;

                // Update nTime every few seconds
                UpdateTime(pblock, pindexPrev);
                if (Params().AllowMinDifficultyBlocks())
                {
                    // Changing pblock->nTime can change work required on testnet:
                    hashTarget.SetCompact(pblock->nBits);
                }
            }
        }
    }
    catch (const boost::thread_interrupted&)
    {
        LogPrintf("HivemindMiner terminated\n");
        throw;
    }
}

void GenerateHiveminds(bool fGenerate, CWallet* pwallet, int nThreads)
{
    static boost::thread_group* minerThreads = NULL;

    if (nThreads < 0) {
        // In regtest threads defaults to 1
        if (Params().DefaultMinerThreads())
            nThreads = Params().DefaultMinerThreads();
        else
            nThreads = boost::thread::hardware_concurrency();
    }

    if (minerThreads != NULL)
    {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)
        minerThreads->create_thread(boost::bind(&HivemindMiner, pwallet));
}

#endif // ENABLE_WALLET
