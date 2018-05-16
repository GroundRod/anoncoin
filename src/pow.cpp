// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <cmath>

#include <pow.h>

#include <arith_uint256.h>
#include <chain.h>
#include <primitives/block.h>
#include <uint256.h>
#include <chainparams.h>
#include <util.h>
#include <miner.h>

#include <stdint.h>

using namespace std;


static arith_uint256 bnProofOfWorkLimit(~arith_uint256(0) >> 20);

#define TIPFILTERBLOCKS_DEFAULT "21"
#define USESHEADER_DEFAULT false
#define NMAXDIFFINCREASE "200"
#define NMAXDIFFDECREASE "170"
#define NMAXDIFFINCREASE2 150
#define NMAXDIFFDECREASE2 130
#define DMININTEGRATOR 170
#define DMININTEGRATOR2 176
#define DMAXINTEGRATOR 190
#define DMAXINTEGRATOR2 195
#define WEIGHTEDAVGTIPBLOCKS_UP 9
#define WEIGHTEDAVGTIPBLOCKS_DOWN 20
#define PID_PROPORTIONALGAIN2 1.6
#define PID_INTEGRATORTIME2 129600
#define PID_INTEGRATORGAIN2 8   
#define PID_DERIVATIVEGAIN2 3


//! This procedure averages the miner threads 10 second samples, as found from over the last 10 minutes.
//! Once the totals from each thread have been created, it finds the average each of them are producing
//! and sums each of those, into the total returned.
double GetFastMiningKHPS()
{
    double dFastKHPS = 0.0;

    return dFastKHPS;
}

// #define LOG_DEBUG_OUTPUT

//! This value defines the Anoncoin block rate production, difficulty calculations and Proof Of Work functions all use this as the goal...
//! Anoncoin uses 3 minute spacing, so nTargetSpacing = 180 seconds, and has since the KGW era, it serves no purpose to make it a variable,
//! as PID parameter gains are different for each and every TargetSpacing value.
//! Any program using this constant, now needs to include pow.h to access it, as this is the primary place where its value is of major importance.
const int64_t nTargetSpacing = 180;

//! Difficulty Protocols have changed over the years, at specific points in Anoncoin's history the following SwitchHeight values are those blocks
//! where an event occurred which required changing the way things are calculated. aka HARDFORK
static const int nDifficultySwitchHeight = 15420;   // Protocol 1 happened here
static const int nDifficultySwitchHeight2 = 77777;  // Protocol 2 starts at this block
static const int nDifficultySwitchHeight3 = 87777;  // Protocol 3 began the KGW era
static const int32_t nDifficultySwitchHeight4 = 555555;

//! The master Retarget PID pointer and all its operations are protected by a CritialSection LOCK,
//! the pointer itself is initialized to NULL upon program load, and until initialized should allow
//! any unit testing software to run without producing segment faults or other types of software
//! failure.  It will not be able to provide a retarget goal for any value other than the minimum
//! network difficulty, but that would be true for any retarget system having no blockchain or at
//! most the genesis block.  Once initialized it runs when needed, as fast as possible, and from
//! whatever thread is calling it for a GetNextWorkRequired() result...GR
static CCriticalSection cs_retargetpid;
CRetargetPidController *pRetargetPid = NULL;

unsigned int GetNextWorkRequired_Bitcoin(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    assert(pindexLast != nullptr);
    unsigned int nProofOfWorkLimit = UintToArith256(params.powLimit).GetCompact();

    // Only change once per difficulty adjustment interval
    if ((pindexLast->nHeight+1) % params.DifficultyAdjustmentInterval() != 0)
    {
        if (params.fPowAllowMinDifficultyBlocks)
        {
            // Special difficulty rule for testnet:
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->GetBlockTime() > pindexLast->GetBlockTime() + params.nPowTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % params.DifficultyAdjustmentInterval() != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // Anoncoin: This fixes an issue where a 51% attack can change difficulty at will.
    // Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
    int blockstogoback = params.DifficultyAdjustmentInterval()-1;
    if ((pindexLast->nHeight+1) != params.DifficultyAdjustmentInterval())
        blockstogoback = params.DifficultyAdjustmentInterval();

    // Go back by what we want to be 14 days worth of blocks
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < blockstogoback; i++)
        pindexFirst = pindexFirst->pprev;

    return CalculateNextWorkRequired(pindexLast, pindexFirst->GetBlockTime(), params);
}


unsigned int static KimotoGravityWell(const CBlockIndex* pindexLast, const CBlockHeader *pblock, uint64_t TargetBlocksSpacingSeconds, uint64_t PastBlocksMin, uint64_t PastBlocksMax) {
    /* current difficulty formula, Gostcoin - kimoto gravity well */
    const CBlockIndex *BlockLastSolved = pindexLast;
    const CBlockIndex *BlockReading = pindexLast;
    const CBlockHeader *BlockCreating = pblock;
                        BlockCreating = BlockCreating;
    uint64_t PastBlocksMass = 0;
    uint64_t PastRateActualSeconds = 0;
    uint64_t PastRateTargetSeconds = 0;
    double PastRateAdjustmentRatio = double(1);
    arith_uint256 PastDifficultyAverage;
    arith_uint256 PastDifficultyAveragePrev;
    double EventHorizonDeviation;
    double EventHorizonDeviationFast;
    double EventHorizonDeviationSlow;

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || (uint64_t)BlockLastSolved->nHeight < PastBlocksMin) { return bnProofOfWorkLimit.GetCompact(); }

    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if (PastBlocksMax > 0 && i > PastBlocksMax) { break; }
        PastBlocksMass++;

        if (i == 1) { PastDifficultyAverage.SetCompact(BlockReading->nBits); }
        else { PastDifficultyAverage = ((arith_uint256().SetCompact(BlockReading->nBits) - PastDifficultyAveragePrev) / i) + PastDifficultyAveragePrev; }
        PastDifficultyAveragePrev = PastDifficultyAverage;

        PastRateActualSeconds = BlockLastSolved->GetBlockTime() - BlockReading->GetBlockTime();
        PastRateTargetSeconds = TargetBlocksSpacingSeconds * PastBlocksMass;
        PastRateAdjustmentRatio = double(1);
        if (PastRateActualSeconds < 0) { PastRateActualSeconds = 0; }
        if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
        PastRateAdjustmentRatio = double(PastRateTargetSeconds) / double(PastRateActualSeconds);
        }
        EventHorizonDeviation = 1 + (0.7084 * pow((double(PastBlocksMass)/double(144)), -1.228));
        EventHorizonDeviationFast = EventHorizonDeviation;
        EventHorizonDeviationSlow = 1 / EventHorizonDeviation;

        if (PastBlocksMass >= PastBlocksMin) {
            if ((PastRateAdjustmentRatio <= EventHorizonDeviationSlow) || (PastRateAdjustmentRatio >= EventHorizonDeviationFast)) { assert(BlockReading); break; }
        }
        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    arith_uint256 bnNew(PastDifficultyAverage);
    if (PastRateActualSeconds != 0 && PastRateTargetSeconds != 0) {
        bnNew *= PastRateActualSeconds;
        bnNew /= PastRateTargetSeconds;
    }
    if (bnNew > bnProofOfWorkLimit) { bnNew = bnProofOfWorkLimit; }

    /// debug print
    printf("Difficulty Retarget - Kimoto Gravity Well\n");
    printf("PastRateAdjustmentRatio = %g\n", PastRateAdjustmentRatio);
    printf("Before: %08x %s\n", BlockLastSolved->nBits, arith_uint256().SetCompact(BlockLastSolved->nBits).ToString().c_str());
    printf("After: %08x %s\n", bnNew.GetCompact(), bnNew.ToString().c_str());

    return bnNew.GetCompact();
}


unsigned int CalculateNextWorkRequired(const CBlockIndex* pindexLast, int64_t nFirstBlockTime, const Consensus::Params& params)
{
    if (params.fPowNoRetargeting)
        return pindexLast->nBits;

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - nFirstBlockTime;
    if (nActualTimespan < params.nPowTargetTimespan/4)
        nActualTimespan = params.nPowTargetTimespan/4;
    if (nActualTimespan > params.nPowTargetTimespan*4)
        nActualTimespan = params.nPowTargetTimespan*4;

    // Retarget
    arith_uint256 bnNew;
    arith_uint256 bnOld;
    bnNew.SetCompact(pindexLast->nBits);
    bnOld = bnNew;
    bool fShift = bnNew.bits() > 235;
    if (fShift)
        bnNew >>= 1;
    bnNew *= nActualTimespan;
    bnNew /= params.nPowTargetTimespan;
    if (fShift)
        bnNew <<= 1;

    const arith_uint256 bnPowLimit = UintToArith256(params.powLimit);
    if (bnNew > bnPowLimit)
        bnNew = bnPowLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, const Consensus::Params& params)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    // Check range
    if (fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(params.powLimit))
        return false;

    // Check proof of work matches claimed amount
    if (UintToArith256(hash) > bnTarget)
        return false;

    return true;
}


unsigned int GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlockHeader *pblock, const Consensus::Params& params)
{
    static const int64_t        BlocksTargetSpacing  = 2.5 * 60; // 2.5 minutes
    unsigned int                TimeDaySeconds       = 60 * 60 * 24;
    int64_t                     PastSecondsMin       = TimeDaySeconds * 0.25;
    int64_t                     PastSecondsMax       = TimeDaySeconds * 7;
    uint64_t                    PastBlocksMin        = PastSecondsMin / BlocksTargetSpacing;
    uint64_t                    PastBlocksMax        = PastSecondsMax / BlocksTargetSpacing;


	int nHeight = pindexLast->nHeight + 1;
	/*if (nHeight < 26754) {
	    return GetNextWorkRequired_Bitcoin(pindexLast, pblock, params);
	}
	else */
    if (nHeight < params.AIP09Height) {
        return GetNextWorkRequired2(pindexLast, pblock, params);
    }
    if (nHeight == params.AIP09Height) {
   	    return 0x1e0ffff0;
	}
    return KimotoGravityWell(pindexLast, pblock, BlocksTargetSpacing, PastBlocksMin, PastBlocksMax);
}

#ifndef SECONDSPERDAY
#define SECONDSPERDAY (60 * 60 * 24)
#endif





//! Primary routine and methodology used to convert an unsigned 256bit value to something
//! small enough which can be converted to a double floating point number and represent
//! Difficulty in a meaningful way.  The only value not allowed is 0.  This process inverts
//! Difficulty value, which is normally thought of as getting harder as the value gets
//! smaller.  Larger proof values, indicate harder difficulty, and visa-versa...
arith_uint256 GetWorkProof(const arith_uint256& uintTarget)
{
    // We need to compute 2**256 / (Target+1), but we can't represent 2**256
    // as it's too large for a uint256. However, as 2**256 is at least as large
    // as Target+1, it is equal to ((2**256 - Target - 1) / (Target+1)) + 1,
    // or ~Target / (Target+1) + 1.
    return uintTarget.size() > 0 ? (~uintTarget / (uintTarget + 1)) + 1 : arith_uint256(0);
}

//! Block proofs are based on the nBits field found within the block, not the actual hash
arith_uint256 GetBlockProof(const CBlockIndex& block)
{
    arith_uint256 bnTarget;
    bool fNegative;
    bool fOverflow;

    bnTarget.SetCompact(block.nBits, &fNegative, &fOverflow);
    return (!fNegative && !fOverflow) ? GetWorkProof( bnTarget ) : arith_uint256(0);
}

//! The primary method of providing difficulty to the user requires the use of a logarithm scale, that can not be done
//! for 256bit numbers unless they are first converted to a work proof, then its value can be returned as a double
//! floating point value which is meaningful.
double GetLog2Work( const arith_uint256& uintDifficulty )
{
    double dResult = 0.0;   //! Return zero, if there are any errors

    arith_uint256 uintWorkProof = GetWorkProof( uintDifficulty );
    if( uintWorkProof.size() > 0 ) {
        double dWorkProof = uintWorkProof.getdouble();
        dResult = (dWorkProof != 0.0) ? log(dWorkProof) / log(2.0) : 0.0;
    }
    return dResult;
}

//! The secondary method of providing difficulty to the user is linear and based on the minimum work required.
//! It is done in such a way that 3 digits of precision to the right of the decimal point is produced in the
//! result returned.
//! NOTE: The largest possible 256bit Number needs to be less than this PowLimit x the multiplier, or overflow will result
//! Once initialized with the main or testnet limit, and as only Scrypt mining is currently being used, the 256bit big
//! number x 1000 calculation need not be done over and over, so we detect that and store it for later use.
double GetLinearWork( const arith_uint256& uintDifficulty, const arith_uint256& uintPowLimit )
{
    static arith_uint256 uintPowLimitLast;
    static arith_uint256 uintPowLimitX1K;

    //! This adds the storage cost of 64 bytes & a time cost of one 256 bit comparison, but stops the needless calculation
    //! of a 256 bit multiply by an unsigned integer that never changes. Once multi-algo implementations are used, that will
    //! not always be the case.
    if( uintPowLimit != uintPowLimitLast ) {
        uintPowLimitX1K = uintPowLimit * 1000;
        uintPowLimitLast = uintPowLimit;
    }
    //! Reducing the calculation required to one 256 bit divide, conversion of that to double, and then one double divide...
    return arith_uint256(uintPowLimitX1K / uintDifficulty).getdouble() / 1000.0;
}

/**
 * The primary routine which verifies a blocks claim of Proof Of Work
 */
bool CheckProofOfWork(const arith_uint256& hash, unsigned int nBits)
{
    bool fNegative;
    bool fOverflow;
    arith_uint256 bnTarget;

    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);

    //! Check range of the Target Difficulty value stored in a block
    if( fNegative || bnTarget == 0 || fOverflow || bnTarget > UintToArith256(Params().GetConsensus().powLimit) )
        return error("CheckProofOfWork() : nBits below minimum work");

    //! Check the proof of work matches claimed amount
    if (hash > bnTarget) {
        //! There is one possibility where this is allowed, if this is TestNet and this hash is better than the minimum
        //! and the Target hash (as found in the block) is equal to the starting difficulty.  Then we assume this check
        //! is being called for one of the first mocktime blocks used to initialize the chain.
        bool fTestNet = !Params().isMainNetwork();
        arith_uint256 uintStartingDiff;
        if( fTestNet ) uintStartingDiff = pRetargetPid->GetTestNetStartingDifficulty();
        if( !fTestNet || hash > UintToArith256(Params().GetConsensus().powLimit) || uintStartingDiff.GetCompact() != nBits ) {
            LogPrintf( "%s : Failed. ", __func__ );
            if( fTestNet ) LogPrintf( "StartingDiff=0x%s ", uintStartingDiff.ToString() );
            LogPrintf( "Target=0x%s hash=0x%s\n", bnTarget.ToString(), hash.ToString() );
            return error("CheckProofOfWork() : hash doesn't match nBits");
        }
    }

    return true;
}

//! Return average network hashes per second based on the last 'lookup' blocks, a minimum of 2 are required.
int64_t CalcNetworkHashPS( const CBlockIndex* pBI, int32_t nLookup )
{
    if( !pBI || !pBI->nHeight )
        return 0;

    //! If lookup is anything less than 2, use 2 blocks, it will not be a very good calculation
    if( nLookup < 2 ) nLookup = 2;

    //! If lookup is not > than the given block index (chain) height selected, then we can not
    //! possibly do the calculation for more than that height...
    if (nLookup > pBI->nHeight) nLookup = pBI->nHeight;

    int64_t minTime = pBI->GetBlockTime();
    int64_t maxTime = minTime;
    int64_t nTime;
    const CBlockIndex* pBI0 = pBI;
    //! Added null pointer safety & do not include the genesis block time for testnet, or the timespan is so large it calculates zero
    for( int32_t i = 1; i < nLookup && pBI0->nHeight > 1; i++ ) {
        pBI0 = pBI0->pprev;
        nTime = pBI0->GetBlockTime();
        if( nTime < minTime ) minTime = nTime;
        if( nTime > maxTime ) maxTime = nTime;
    }

    //! In case there's a situation where minTime == maxTime, we don't want a divide by zero exception.
    if (minTime == maxTime)
        return 0;

    //! The calculation will be wrong by one time difference, because spacing is 1 less than the number of blocks, unless we carefully
    //! scale the values before dividing them.  Chain work proofs have already been calculated for us, and are in the index, so we use them.
    arith_uint256 uintWorkDiff = pBI->nChainWork - pBI0->nChainWork;
    double dWorkDiff = uintWorkDiff.getdouble() / (double)nLookup;
    double dTimeDiff = (double)( maxTime - minTime ) / (double)(nLookup - 1);

    return roundint64( dWorkDiff / dTimeDiff );
}

/**
 *  Difficulty formula, Anoncoin - From the early months, when blocks were very new...
 */
static arith_uint256 OriginalGetNextWorkRequired(const CBlockIndex* pindexLast)
{
    //! These legacy values define the Anoncoin block rate production and are used in this difficulty calculation only...
    static const int64_t nLegacyTargetSpacing = 205;    //! Originally 3.42 minutes * 60 secs was Anoncoin spacing target in seconds
    static const int64_t nLegacyTargetTimespan = 86184; //! in Seconds - Anoncoin legacy value is ~23.94hrs, it came from a
                                                        //! 420 blocks * 205.2 seconds/block calculation, now used only in original NextWorkRequired function.

    //! Anoncoin difficulty adjustment protocol switch (Thanks to FeatherCoin for this idea)
    static const int newTargetTimespan = 2050;              //! For when another adjustment in the timespan was made
    int nHeight = pindexLast->nHeight + 1;
    bool fNewDifficultyProtocol = nHeight >= nDifficultySwitchHeight;
    bool fNewDifficultyProtocol2 = false;
    int64_t nTargetTimespanCurrent = nLegacyTargetTimespan;
    int64_t nLegacyInterval;

    if( nHeight >= nDifficultySwitchHeight2 ) {         //! Jump back to sqrt(2) as the factor of adjustment.
        fNewDifficultyProtocol2 = true;
        fNewDifficultyProtocol = false;
    }

    if( fNewDifficultyProtocol ) nTargetTimespanCurrent *= 4;
    if (fNewDifficultyProtocol2) nTargetTimespanCurrent = newTargetTimespan;
    nLegacyInterval = nTargetTimespanCurrent / nLegacyTargetSpacing;

    //! Only change once per interval, or at protocol switch height
    if( nHeight % nLegacyInterval != 0 && !fNewDifficultyProtocol2 && nHeight != nDifficultySwitchHeight )
        return arith_uint256().SetCompact( pindexLast->nBits );

    //! Anoncoin: This fixes an issue where a 51% attack can change difficulty at will.
    //! Go back the full period unless it's the first retarget after genesis. Code courtesy of Art Forz
    int blockstogoback = nLegacyInterval-1;
    if ((pindexLast->nHeight+1) != nLegacyInterval)
        blockstogoback = nLegacyInterval;

    //! Go back by what we want to be 14 days worth of blocks
    const CBlockIndex* pindexFirst = pindexLast;
    blockstogoback = fNewDifficultyProtocol2 ? (newTargetTimespan/205) : blockstogoback;
    for (int i = 0; pindexFirst && i < blockstogoback; i++)
        pindexFirst = pindexFirst->pprev;
    assert(pindexFirst);

    //! Limit adjustment step
    int64_t nNewSetpoint = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
    int64_t nAveragedTimespanMax = fNewDifficultyProtocol ? (nTargetTimespanCurrent*4) : ((nTargetTimespanCurrent*99)/70);
    int64_t nAveragedTimespanMin = fNewDifficultyProtocol ? (nTargetTimespanCurrent/4) : ((nTargetTimespanCurrent*70)/99);
    if (pindexLast->nHeight+1 >= nDifficultySwitchHeight2) {
        if (nNewSetpoint < nAveragedTimespanMin)
            nNewSetpoint = nAveragedTimespanMin;
        if (nNewSetpoint > nAveragedTimespanMax)
            nNewSetpoint = nAveragedTimespanMax;
    } else if (pindexLast->nHeight+1 > nDifficultySwitchHeight) {
        if (nNewSetpoint < nAveragedTimespanMin/4)
            nNewSetpoint = nAveragedTimespanMin/4;
        if (nNewSetpoint > nAveragedTimespanMax)
            nNewSetpoint = nAveragedTimespanMax;
    } else {
        if (nNewSetpoint < nAveragedTimespanMin)
            nNewSetpoint = nAveragedTimespanMin;
        if (nNewSetpoint > nAveragedTimespanMax)
            nNewSetpoint = nAveragedTimespanMax;
    }

    //! Retarget
    arith_uint256 bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nNewSetpoint;
    bnNew /= fNewDifficultyProtocol2 ? nTargetTimespanCurrent : nLegacyTargetTimespan;
    // debug print
#if defined( LOG_DEBUG_OUTPUT )
    LogPrintf("Difficulty Retarget - pre-Kimoto Gravity Well era\n");
    LogPrintf("  TargetTimespan = %lld    ActualTimespan = %lld\n", nLegacyTargetTimespan, nNewSetpoint);
    LogPrintf("  Before: %08x  %s\n", pindexLast->nBits, arith_uint256().SetCompact(pindexLast->nBits).ToString());
    LogPrintf("  After : %08x  %s\n", bnNew.GetCompact(), bnNew.ToString());
#endif
    const arith_uint256 &uintPOWlimit = UintToArith256(Params().GetConsensus().powLimit);
    if( bnNew > uintPOWlimit ) {
        LogPrintf("Block at Height %d, Computed Next Work Required %0x limited and set to Minimum %0x\n", nHeight, bnNew.GetCompact(), uintPOWlimit.GetCompact());
        bnNew = uintPOWlimit;
    }
    return bnNew;
}






/**
 *  Difficulty formula, Anoncoin - kimoto gravity well, use of it continues into 2015
 */
static const int32_t nMinBlocksToAvg = (SECONDSPERDAY * 0.25) / nTargetSpacing;
static const int32_t nMaxBlocksToAvg = (SECONDSPERDAY * 7) / nTargetSpacing;

static const double kgw_blockmass_curve[ nMaxBlocksToAvg ] = {
    317.7726609803, 136.2330546899, 83.1945064501, 58.7321888328, 44.8947447056, 36.0895628164, 30.0380400348, 25.6463827580, 22.3273988660, 19.7390552992, 17.6693043262,
    15.9800451614, 14.5776702210, 13.3965964865, 12.3895778306, 11.5217590972, 10.7668927183, 10.1048554768, 9.5199740272, 8.9998686827, 8.5346381268, 8.1162736601,
    7.7382312296, 7.3951139606, 7.0824333982, 6.7964276774, 6.5339214509, 6.2922168392, 6.0690077054, 5.8623116556, 5.6704156486, 5.4918321519, 5.3252635430,
    5.1695730087, 5.0237606053, 4.8869434466, 4.7583392165, 4.6372523753, 4.5230625620, 4.4152147970, 4.3132111693, 4.2166037521, 4.1249885424, 4.0380002557,
    3.9553078399, 3.8766105938, 3.8016347999, 3.7301307926, 3.6618703977, 3.5966446910, 3.5342620289, 3.4745463156, 3.4173354720, 3.3624800824, 3.3098421927,
    3.2592942428, 3.2107181155, 3.1640042866, 3.1190510651, 3.0757639122, 3.0340548292, 2.9938418072, 2.9550483309, 2.9176029307, 2.8814387776, 2.8464933162,
    2.8127079319, 2.7800276481, 2.7484008517, 2.7177790416, 2.6881166003, 2.6593705848, 2.6315005352, 2.6044682999, 2.5782378745, 2.5527752549, 2.5280483009,
    2.5040266122, 2.4806814130, 2.4579854461, 2.4359128751, 2.4144391939, 2.3935411430, 2.3731966319, 2.3533846674, 2.3340852874, 2.3152794981, 2.2969492173,
    2.2790772209, 2.2616470926, 2.2446431780, 2.2280505412, 2.2118549245, 2.1960427107, 2.1806008879, 2.1655170166, 2.1507791989, 2.1363760495, 2.1222966688,
    2.1085306176, 2.0950678930, 2.0818989066, 2.0690144628, 2.0564057397, 2.0440642699, 2.0319819238, 2.0201508924, 2.0085636721, 1.9972130502, 1.9860920908,
    1.9751941219, 1.9645127233, 1.9540417145, 1.9437751443, 1.9337072798, 1.9238325970, 1.9141457712, 1.9046416686, 1.8953153372, 1.8861619996, 1.8771770451,
    1.8683560226, 1.8596946340, 1.8511887273, 1.8428342912, 1.8346274487, 1.8265644516, 1.8186416757, 1.8108556152, 1.8032028784, 1.7956801830, 1.7882843514,
    1.7810123075, 1.7738610716, 1.7668277577, 1.7599095690, 1.7531037955, 1.7464078095, 1.7398190638, 1.7333350876, 1.7269534843, 1.7206719286, 1.7144881640,
    1.7084000000, 1.7024053100, 1.6965020290, 1.6906881512, 1.6849617283, 1.6793208670, 1.6737637274, 1.6682885212, 1.6628935095, 1.6575770019, 1.6523373539,
    1.6471729660, 1.6420822822, 1.6370637881, 1.6321160099, 1.6272375130, 1.6224269006, 1.6176828127, 1.6130039246, 1.6083889460, 1.6038366199, 1.5993457214,
    1.5949150568, 1.5905434627, 1.5862298048, 1.5819729773, 1.5777719017, 1.5736255264, 1.5695328254, 1.5654927979, 1.5615044674, 1.5575668809, 1.5536791082,
    1.5498402416, 1.5460493946, 1.5423057020, 1.5386083185, 1.5349564188, 1.5313491967, 1.5277858647, 1.5242656531, 1.5207878101, 1.5173516009, 1.5139563071,
    1.5106012268, 1.5072856734, 1.5040089759, 1.5007704779, 1.4975695376, 1.4944055273, 1.4912778327, 1.4881858531, 1.4851290006, 1.4821067000, 1.4791183883,
    1.4761635145, 1.4732415391, 1.4703519342, 1.4674941828, 1.4646677786, 1.4618722258, 1.4591070390, 1.4563717427, 1.4536658708, 1.4509889671, 1.4483405843,
    1.4457202843, 1.4431276378, 1.4405622237, 1.4380236298, 1.4355114516, 1.4330252928, 1.4305647647, 1.4281294862, 1.4257190836, 1.4233331906, 1.4209714476,
    1.4186335020, 1.4163190081, 1.4140276266, 1.4117590244, 1.4095128751, 1.4072888581, 1.4050866587, 1.4029059683, 1.4007464838, 1.3986079078, 1.3964899481,
    1.3943923182, 1.3923147365, 1.3902569265, 1.3882186168, 1.3861995409, 1.3841994368, 1.3822180474, 1.3802551201, 1.3783104065, 1.3763836630, 1.3744746499,
    1.3725831317, 1.3707088772, 1.3688516591, 1.3670112537, 1.3651874416, 1.3633800068, 1.3615887372, 1.3598134241, 1.3580538623, 1.3563098503, 1.3545811897,
    1.3528676854, 1.3511691458, 1.3494853822, 1.3478162091, 1.3461614440, 1.3445209075, 1.3428944229, 1.3412818166, 1.3396829176, 1.3380975578, 1.3365255716,
    1.3349667963, 1.3334210716, 1.3318882398, 1.3303681457, 1.3288606365, 1.3273655617, 1.3258827734, 1.3244121257, 1.3229534752, 1.3215066807, 1.3200716029,
    1.3186481050, 1.3172360520, 1.3158353111, 1.3144457514, 1.3130672442, 1.3116996625, 1.3103428812, 1.3089967773, 1.3076612295, 1.3063361181, 1.3050213256,
    1.3037167358, 1.3024222344, 1.3011377089, 1.2998630482, 1.2985981429, 1.2973428852, 1.2960971689, 1.2948608893, 1.2936339431, 1.2924162287, 1.2912076456,
    1.2900080950, 1.2888174795, 1.2876357029, 1.2864626705, 1.2852982888, 1.2841424658, 1.2829951106, 1.2818561336, 1.2807254464, 1.2796029619, 1.2784885942,
    1.2773822586, 1.2762838713, 1.2751933500, 1.2741106133, 1.2730355809, 1.2719681737, 1.2709083136, 1.2698559234, 1.2688109272, 1.2677732499, 1.2667428174,
    1.2657195568, 1.2647033960, 1.2636942637, 1.2626920897, 1.2616968049, 1.2607083407, 1.2597266297, 1.2587516051, 1.2577832013, 1.2568213533, 1.2558659970,
    1.2549170690, 1.2539745070, 1.2530382492, 1.2521082348, 1.2511844035, 1.2502666960, 1.2493550537, 1.2484494186, 1.2475497337, 1.2466559423, 1.2457679887,
    1.2448858179, 1.2440093755, 1.2431386076, 1.2422734613, 1.2414138841, 1.2405598242, 1.2397112305, 1.2388680524, 1.2380302399, 1.2371977438, 1.2363705153,
    1.2355485063, 1.2347316692, 1.2339199569, 1.2331133230, 1.2323117217, 1.2315151075, 1.2307234356, 1.2299366617, 1.2291547421, 1.2283776334, 1.2276052929,
    1.2268376783, 1.2260747479, 1.2253164603, 1.2245627748, 1.2238136508, 1.2230690487, 1.2223289289, 1.2215932524, 1.2208619807, 1.2201350757, 1.2194124997,
    1.2186942155, 1.2179801862, 1.2172703754, 1.2165647471, 1.2158632657, 1.2151658959, 1.2144726030, 1.2137833524, 1.2130981102, 1.2124168427, 1.2117395165,
    1.2110660986, 1.2103965566, 1.2097308581, 1.2090689713, 1.2084108646, 1.2077565069, 1.2071058673, 1.2064589151, 1.2058156204, 1.2051759530, 1.2045398836,
    1.2039073828, 1.2032784218, 1.2026529718, 1.2020310046, 1.2014124922, 1.2007974067, 1.2001857208, 1.1995774073, 1.1989724393, 1.1983707903, 1.1977724339,
    1.1971773441, 1.1965854951, 1.1959968614, 1.1954114177, 1.1948291391, 1.1942500009, 1.1936739784, 1.1931010477, 1.1925311845, 1.1919643653, 1.1914005665,
    1.1908397648, 1.1902819372, 1.1897270609, 1.1891751134, 1.1886260723, 1.1880799155, 1.1875366210, 1.1869961672, 1.1864585327, 1.1859236961, 1.1853916365,
    1.1848623329, 1.1843357648, 1.1838119116, 1.1832907533, 1.1827722697, 1.1822564410, 1.1817432475, 1.1812326698, 1.1807246886, 1.1802192848, 1.1797164396,
    1.1792161341, 1.1787183499, 1.1782230686, 1.1777302720, 1.1772399420, 1.1767520609, 1.1762666109, 1.1757835746, 1.1753029345, 1.1748246735, 1.1743487746,
    1.1738752209, 1.1734039958, 1.1729350825, 1.1724684649, 1.1720041265, 1.1715420513, 1.1710822234, 1.1706246270, 1.1701692464, 1.1697160660, 1.1692650706,
    1.1688162449, 1.1683695739, 1.1679250424, 1.1674826358, 1.1670423394, 1.1666041385, 1.1661680188, 1.1657339661, 1.1653019660, 1.1648720047, 1.1644440681,
    1.1640181425, 1.1635942143, 1.1631722698, 1.1627522957, 1.1623342787, 1.1619182056, 1.1615040633, 1.1610918389, 1.1606815194, 1.1602730923, 1.1598665448,
    1.1594618645, 1.1590590390, 1.1586580559, 1.1582589031, 1.1578615686, 1.1574660402, 1.1570723063, 1.1566803549, 1.1562901745, 1.1559017534, 1.1555150801,
    1.1551301434, 1.1547469320, 1.1543654346, 1.1539856401, 1.1536075377, 1.1532311163, 1.1528563652, 1.1524832736, 1.1521118310, 1.1517420268, 1.1513738505,
    1.1510072917, 1.1506423403, 1.1502789860, 1.1499172187, 1.1495570284, 1.1491984051, 1.1488413390, 1.1484858203, 1.1481318394, 1.1477793865, 1.1474284521,
    1.1470790269, 1.1467311014, 1.1463846662, 1.1460397123, 1.1456962303, 1.1453542113, 1.1450136462, 1.1446745260, 1.1443368420, 1.1440005853, 1.1436657472,
    1.1433323190, 1.1430002922, 1.1426696582, 1.1423404087, 1.1420125351, 1.1416860292, 1.1413608827, 1.1410370875, 1.1407146355, 1.1403935185, 1.1400737286,
    1.1397552578, 1.1394380984, 1.1391222424, 1.1388076821, 1.1384944098, 1.1381824180, 1.1378716990, 1.1375622454, 1.1372540496, 1.1369471043, 1.1366414022,
    1.1363369360, 1.1360336983, 1.1357316822, 1.1354308804, 1.1351312859, 1.1348328917, 1.1345356908, 1.1342396764, 1.1339448415, 1.1336511794, 1.1333586834,
    1.1330673467, 1.1327771627, 1.1324881248, 1.1322002264, 1.1319134611, 1.1316278224, 1.1313433039, 1.1310598992, 1.1307776021, 1.1304964063, 1.1302163055,
    1.1299372937, 1.1296593646, 1.1293825122, 1.1291067305, 1.1288320135, 1.1285583552, 1.1282857497, 1.1280141912, 1.1277436739, 1.1274741919, 1.1272057397,
    1.1269383113, 1.1266719013, 1.1264065040, 1.1261421138, 1.1258787253, 1.1256163329, 1.1253549311, 1.1250945146, 1.1248350780, 1.1245766160, 1.1243191232,
    1.1240625944, 1.1238070244, 1.1235524080, 1.1232987401, 1.1230460155, 1.1227942292, 1.1225433761, 1.1222934513, 1.1220444497, 1.1217963664, 1.1215491966,
    1.1213029354, 1.1210575778, 1.1208131193, 1.1205695548, 1.1203268799, 1.1200850897, 1.1198441795, 1.1196041448, 1.1193649809, 1.1191266834, 1.1188892475,
    1.1186526689, 1.1184169430, 1.1181820655, 1.1179480318, 1.1177148376, 1.1174824786, 1.1172509504, 1.1170202487, 1.1167903692, 1.1165613078, 1.1163330602,
    1.1161056222, 1.1158789897, 1.1156531585, 1.1154281246, 1.1152038839, 1.1149804322, 1.1147577657, 1.1145358803, 1.1143147721, 1.1140944371, 1.1138748714,
    1.1136560711, 1.1134380323, 1.1132207513, 1.1130042243, 1.1127884473, 1.1125734168, 1.1123591288, 1.1121455798, 1.1119327661, 1.1117206839, 1.1115093297,
    1.1112986998, 1.1110887906, 1.1108795986, 1.1106711203, 1.1104633520, 1.1102562904, 1.1100499319, 1.1098442730, 1.1096393104, 1.1094350407, 1.1092314604,
    1.1090285661, 1.1088263546, 1.1086248225, 1.1084239666, 1.1082237834, 1.1080242698, 1.1078254226, 1.1076272384, 1.1074297142, 1.1072328466, 1.1070366327,
    1.1068410692, 1.1066461530, 1.1064518810, 1.1062582502, 1.1060652574, 1.1058728996, 1.1056811739, 1.1054900772, 1.1052996064, 1.1051097588, 1.1049205312,
    1.1047319208, 1.1045439247, 1.1043565399, 1.1041697636, 1.1039835930, 1.1037980251, 1.1036130572, 1.1034286865, 1.1032449101, 1.1030617254, 1.1028791295,
    1.1026971196, 1.1025156932, 1.1023348475, 1.1021545797, 1.1019748873, 1.1017957675, 1.1016172177, 1.1014392353, 1.1012618177, 1.1010849624, 1.1009086666,
    1.1007329279, 1.1005577436, 1.1003831114, 1.1002090286, 1.1000354928, 1.0998625015, 1.0996900521, 1.0995181423, 1.0993467696, 1.0991759316, 1.0990056259,
    1.0988358500, 1.0986666015, 1.0984978782, 1.0983296776, 1.0981619975, 1.0979948354, 1.0978281891, 1.0976620562, 1.0974964345, 1.0973313217, 1.0971667155,
    1.0970026137, 1.0968390140, 1.0966759143, 1.0965133123, 1.0963512058, 1.0961895926, 1.0960284706, 1.0958678375, 1.0957076914, 1.0955480299, 1.0953888510,
    1.0952301527, 1.0950719326, 1.0949141889, 1.0947569195, 1.0946001221, 1.0944437949, 1.0942879358, 1.0941325427, 1.0939776136, 1.0938231465, 1.0936691395,
    1.0935155905, 1.0933624976, 1.0932098588, 1.0930576722, 1.0929059358, 1.0927546477, 1.0926038059, 1.0924534087, 1.0923034540, 1.0921539401, 1.0920048649,
    1.0918562267, 1.0917080236, 1.0915602538, 1.0914129155, 1.0912660067, 1.0911195257, 1.0909734707, 1.0908278399, 1.0906826316, 1.0905378438, 1.0903934750,
    1.0902495233, 1.0901059870, 1.0899628643, 1.0898201535, 1.0896778530, 1.0895359610, 1.0893944757, 1.0892533956, 1.0891127190, 1.0889724441, 1.0888325693,
    1.0886930929, 1.0885540134, 1.0884153291, 1.0882770383, 1.0881391395, 1.0880016310, 1.0878645113, 1.0877277787, 1.0875914316, 1.0874554686, 1.0873198880,
    1.0871846882, 1.0870498678, 1.0869154251, 1.0867813587, 1.0866476671, 1.0865143486, 1.0863814019, 1.0862488253, 1.0861166175, 1.0859847769, 1.0858533020,
    1.0857221914, 1.0855914437, 1.0854610573, 1.0853310308, 1.0852013629, 1.0850720520, 1.0849430967, 1.0848144956, 1.0846862474, 1.0845583506, 1.0844308037,
    1.0843036056, 1.0841767546, 1.0840502496, 1.0839240890, 1.0837982717, 1.0836727961, 1.0835476610, 1.0834228650, 1.0832984068, 1.0831742851, 1.0830504986,
    1.0829270459, 1.0828039257, 1.0826811368, 1.0825586779, 1.0824365476, 1.0823147447, 1.0821932680, 1.0820721161, 1.0819512878, 1.0818307819, 1.0817105971,
    1.0815907322, 1.0814711859, 1.0813519570, 1.0812330444, 1.0811144467, 1.0809961628, 1.0808781914, 1.0807605315, 1.0806431817, 1.0805261410, 1.0804094080,
    1.0802929818, 1.0801768610, 1.0800610446, 1.0799455313, 1.0798303201, 1.0797154097, 1.0796007991, 1.0794864871, 1.0793724726, 1.0792587545, 1.0791453317,
    1.0790322029, 1.0789193673, 1.0788068235, 1.0786945707, 1.0785826076, 1.0784709331, 1.0783595463, 1.0782484460, 1.0781376312, 1.0780271008, 1.0779168537,
    1.0778068889, 1.0776972054, 1.0775878021, 1.0774786779, 1.0773698319, 1.0772612631, 1.0771529703, 1.0770449526, 1.0769372089, 1.0768297384, 1.0767225398,
    1.0766156124, 1.0765089550, 1.0764025667, 1.0762964465, 1.0761905935, 1.0760850065, 1.0759796848, 1.0758746273, 1.0757698331, 1.0756653011, 1.0755610305,
    1.0754570203, 1.0753532696, 1.0752497774, 1.0751465428, 1.0750435649, 1.0749408428, 1.0748383754, 1.0747361620, 1.0746342016, 1.0745324933, 1.0744310361,
    1.0743298293, 1.0742288719, 1.0741281630, 1.0740277017, 1.0739274871, 1.0738275185, 1.0737277948, 1.0736283152, 1.0735290790, 1.0734300851, 1.0733313327,
    1.0732328211, 1.0731345493, 1.0730365165, 1.0729387218, 1.0728411645, 1.0727438437, 1.0726467585, 1.0725499081, 1.0724532918, 1.0723569087, 1.0722607579,
    1.0721648387, 1.0720691502, 1.0719736917, 1.0718784624, 1.0717834614, 1.0716886880, 1.0715941413, 1.0714998207, 1.0714057252, 1.0713118542, 1.0712182068,
    1.0711247824, 1.0710315800, 1.0709385990, 1.0708458386, 1.0707532980, 1.0706609766, 1.0705688734, 1.0704769878, 1.0703853191, 1.0702938665, 1.0702026292,
    1.0701116065, 1.0700207978, 1.0699302022, 1.0698398191, 1.0697496476, 1.0696596872, 1.0695699371, 1.0694803966, 1.0693910649, 1.0693019414, 1.0692130254,
    1.0691243161, 1.0690358129, 1.0689475151, 1.0688594220, 1.0687715328, 1.0686838470, 1.0685963638, 1.0685090826, 1.0684220027, 1.0683351233, 1.0682484440,
    1.0681619638, 1.0680756823, 1.0679895988, 1.0679037126, 1.0678180230, 1.0677325293, 1.0676472311, 1.0675621275, 1.0674772180, 1.0673925018, 1.0673079785,
    1.0672236473, 1.0671395076, 1.0670555588, 1.0669718002, 1.0668882313, 1.0668048513, 1.0667216598, 1.0666386560, 1.0665558394, 1.0664732093, 1.0663907651,
    1.0663085063, 1.0662264323, 1.0661445423, 1.0660628359, 1.0659813124, 1.0658999713, 1.0658188119, 1.0657378337, 1.0656570360, 1.0655764184, 1.0654959801,
    1.0654157207, 1.0653356396, 1.0652557362, 1.0651760099, 1.0650964601, 1.0650170863, 1.0649378879, 1.0648588644, 1.0647800152, 1.0647013398, 1.0646228375,
    1.0645445079, 1.0644663503, 1.0643883644, 1.0643105494, 1.0642329048, 1.0641554302, 1.0640781249, 1.0640009885, 1.0639240204, 1.0638472201, 1.0637705870,
    1.0636941206, 1.0636178205, 1.0635416860, 1.0634657166, 1.0633899119, 1.0633142713, 1.0632387943, 1.0631634803, 1.0630883290, 1.0630133397, 1.0629385120,
    1.0628638454, 1.0627893393, 1.0627149933, 1.0626408068, 1.0625667794, 1.0624929106, 1.0624191998, 1.0623456467, 1.0622722506, 1.0621990112, 1.0621259279,
    1.0620530002, 1.0619802277, 1.0619076100, 1.0618351464, 1.0617628366, 1.0616906800, 1.0616186763, 1.0615468249, 1.0614751253, 1.0614035772, 1.0613321800,
    1.0612609332, 1.0611898365, 1.0611188894, 1.0610480913, 1.0609774419, 1.0609069408, 1.0608365873, 1.0607663812, 1.0606963219, 1.0606264091, 1.0605566422,
    1.0604870208, 1.0604175445, 1.0603482129, 1.0602790255, 1.0602099819, 1.0601410816, 1.0600723242, 1.0600037093, 1.0599352365, 1.0598669054, 1.0597987154,
    1.0597306662, 1.0596627574, 1.0595949885, 1.0595273592, 1.0594598690, 1.0593925175, 1.0593253043, 1.0592582289, 1.0591912910, 1.0591244902, 1.0590578261,
    1.0589912982, 1.0589249061, 1.0588586495, 1.0587925279, 1.0587265410, 1.0586606883, 1.0585949695, 1.0585293841, 1.0584639318, 1.0583986122, 1.0583334249,
    1.0582683694, 1.0582034455, 1.0581386527, 1.0580739907, 1.0580094590, 1.0579450573, 1.0578807852, 1.0578166424, 1.0577526284, 1.0576887428, 1.0576249854,
    1.0575613557, 1.0574978534, 1.0574344780, 1.0573712293, 1.0573081068, 1.0572451103, 1.0571822392, 1.0571194933, 1.0570568723, 1.0569943757, 1.0569320031,
    1.0568697543, 1.0568076289, 1.0567456265, 1.0566837468, 1.0566219894, 1.0565603539, 1.0564988401, 1.0564374476, 1.0563761760, 1.0563150249, 1.0562539941,
    1.0561930832, 1.0561322919, 1.0560716198, 1.0560110665, 1.0559506318, 1.0558903153, 1.0558301167, 1.0557700356, 1.0557100717, 1.0556502247, 1.0555904942,
    1.0555308799, 1.0554713815, 1.0554119987, 1.0553527312, 1.0552935785, 1.0552345404, 1.0551756166, 1.0551168068, 1.0550581105, 1.0549995276, 1.0549410577,
    1.0548827005, 1.0548244556, 1.0547663228, 1.0547083017, 1.0546503921, 1.0545925935, 1.0545349058, 1.0544773286, 1.0544198616, 1.0543625045, 1.0543052569,
    1.0542481187, 1.0541910894, 1.0541341688, 1.0540773566, 1.0540206525, 1.0539640562, 1.0539075673, 1.0538511857, 1.0537949109, 1.0537387428, 1.0536826809,
    1.0536267251, 1.0535708750, 1.0535151304, 1.0534594909, 1.0534039563, 1.0533485262, 1.0532932005, 1.0532379788, 1.0531828608, 1.0531278462, 1.0530729348,
    1.0530181263, 1.0529634204, 1.0529088168, 1.0528543153, 1.0527999156, 1.0527456173, 1.0526914203, 1.0526373242, 1.0525833288, 1.0525294339, 1.0524756390,
    1.0524219441, 1.0523683487, 1.0523148527, 1.0522614557, 1.0522081575, 1.0521549579, 1.0521018565, 1.0520488532, 1.0519959476, 1.0519431395, 1.0518904286,
    1.0518378147, 1.0517852975, 1.0517328768, 1.0516805522, 1.0516283236, 1.0515761907, 1.0515241532, 1.0514722109, 1.0514203635, 1.0513686108, 1.0513169525,
    1.0512653884, 1.0512139182, 1.0511625417, 1.0511112586, 1.0510600688, 1.0510089718, 1.0509579676, 1.0509070558, 1.0508562363, 1.0508055087, 1.0507548728,
    1.0507043284, 1.0506538753, 1.0506035132, 1.0505532418, 1.0505030610, 1.0504529705, 1.0504029701, 1.0503530595, 1.0503032384, 1.0502535068, 1.0502038643,
    1.0501543107, 1.0501048457, 1.0500554692, 1.0500061809, 1.0499569806, 1.0499078681, 1.0498588431, 1.0498099054, 1.0497610547, 1.0497122909, 1.0496636138,
    1.0496150231, 1.0495665185, 1.0495180999, 1.0494697671, 1.0494215198, 1.0493733577, 1.0493252808, 1.0492772888, 1.0492293814, 1.0491815584, 1.0491338197,
    1.0490861649, 1.0490385940, 1.0489911066, 1.0489437026, 1.0488963818, 1.0488491439, 1.0488019887, 1.0487549161, 1.0487079257, 1.0486610175, 1.0486141912,
    1.0485674466, 1.0485207834, 1.0484742015, 1.0484277007, 1.0483812808, 1.0483349416, 1.0482886828, 1.0482425042, 1.0481964058, 1.0481503871, 1.0481044482,
    1.0480585887, 1.0480128085, 1.0479671073, 1.0479214850, 1.0478759413, 1.0478304761, 1.0477850892, 1.0477397804, 1.0476945495, 1.0476493962, 1.0476043205,
    1.0475593221, 1.0475144007, 1.0474695563, 1.0474247887, 1.0473800975, 1.0473354827, 1.0472909441, 1.0472464815, 1.0472020946, 1.0471577834, 1.0471135475,
    1.0470693869, 1.0470253014, 1.0469812907, 1.0469373546, 1.0468934931, 1.0468497058, 1.0468059927, 1.0467623536, 1.0467187882, 1.0466752963, 1.0466318779,
    1.0465885327, 1.0465452606, 1.0465020613, 1.0464589347, 1.0464158806, 1.0463728989, 1.0463299893, 1.0462871517, 1.0462443859, 1.0462016917, 1.0461590690,
    1.0461165176, 1.0460740373, 1.0460316279, 1.0459892893, 1.0459470213, 1.0459048238, 1.0458626965, 1.0458206393, 1.0457786520, 1.0457367345, 1.0456948865,
    1.0456531080, 1.0456113988, 1.0455697586, 1.0455281873, 1.0454866848, 1.0454452509, 1.0454038854, 1.0453625881, 1.0453213590, 1.0452801978, 1.0452391044,
    1.0451980786, 1.0451571202, 1.0451162292, 1.0450754052, 1.0450346483, 1.0449939581, 1.0449533346, 1.0449127777, 1.0448722870, 1.0448318625, 1.0447915041,
    1.0447512115, 1.0447109846, 1.0446708233, 1.0446307274, 1.0445906968, 1.0445507312, 1.0445108306, 1.0444709947, 1.0444312236, 1.0443915168, 1.0443518745,
    1.0443122963, 1.0442727821, 1.0442333319, 1.0441939453, 1.0441546224, 1.0441153628, 1.0440761666, 1.0440370335, 1.0439979634, 1.0439589561, 1.0439200116,
    1.0438811296, 1.0438423100, 1.0438035527, 1.0437648574, 1.0437262242, 1.0436876528, 1.0436491431, 1.0436106949, 1.0435723081, 1.0435339826, 1.0434957182,
    1.0434575148, 1.0434193722, 1.0433812903, 1.0433432689, 1.0433053080, 1.0432674074, 1.0432295669, 1.0431917864, 1.0431540658, 1.0431164048, 1.0430788035,
    1.0430412617, 1.0430037791, 1.0429663557, 1.0429289914, 1.0428916860, 1.0428544394, 1.0428172514, 1.0427801219, 1.0427430508, 1.0427060379, 1.0426690832,
    1.0426321864, 1.0425953474, 1.0425585662, 1.0425218426, 1.0424851764, 1.0424485676, 1.0424120159, 1.0423755213, 1.0423390836, 1.0423027028, 1.0422663786,
    1.0422301110, 1.0421938998, 1.0421577448, 1.0421216461, 1.0420856034, 1.0420496166, 1.0420136856, 1.0419778103, 1.0419419905, 1.0419062262, 1.0418705171,
    1.0418348632, 1.0417992643, 1.0417637204, 1.0417282313, 1.0416927968, 1.0416574169, 1.0416220914, 1.0415868203, 1.0415516033, 1.0415164404, 1.0414813315,
    1.0414462764, 1.0414112750, 1.0413763271, 1.0413414328, 1.0413065918, 1.0412718041, 1.0412370695, 1.0412023878, 1.0411677591, 1.0411331831, 1.0410986598,
    1.0410641890, 1.0410297707, 1.0409954046, 1.0409610908, 1.0409268290, 1.0408926191, 1.0408584611, 1.0408243549, 1.0407903003, 1.0407562971, 1.0407223454,
    1.0406884449, 1.0406545956, 1.0406207974, 1.0405870501, 1.0405533537, 1.0405197079, 1.0404861128, 1.0404525682, 1.0404190740, 1.0403856300, 1.0403522363,
    1.0403188926, 1.0402855988, 1.0402523549, 1.0402191608, 1.0401860162, 1.0401529212, 1.0401198756, 1.0400868793, 1.0400539323, 1.0400210343, 1.0399881853,
    1.0399553852, 1.0399226339, 1.0398899312, 1.0398572772, 1.0398246716, 1.0397921143, 1.0397596053, 1.0397271445, 1.0396947317, 1.0396623669, 1.0396300499,
    1.0395977806, 1.0395655590, 1.0395333850, 1.0395012583, 1.0394691790, 1.0394371470, 1.0394051621, 1.0393732242, 1.0393413332, 1.0393094891, 1.0392776917,
    1.0392459410, 1.0392142368, 1.0391825790, 1.0391509676, 1.0391194024, 1.0390878833, 1.0390564103, 1.0390249833, 1.0389936021, 1.0389622666, 1.0389309768,
    1.0388997326, 1.0388685338, 1.0388373804, 1.0388062723, 1.0387752093, 1.0387441915, 1.0387132186, 1.0386822906, 1.0386514074, 1.0386205690, 1.0385897751,
    1.0385590258, 1.0385283208, 1.0384976603, 1.0384670439, 1.0384364717, 1.0384059436, 1.0383754594, 1.0383450191, 1.0383146226, 1.0382842697, 1.0382539605,
    1.0382236948, 1.0381934725, 1.0381632935, 1.0381331577, 1.0381030651, 1.0380730156, 1.0380430090, 1.0380130453, 1.0379831244, 1.0379532462, 1.0379234106,
    1.0378936176, 1.0378638669, 1.0378341587, 1.0378044927, 1.0377748688, 1.0377452871, 1.0377157474, 1.0376862496, 1.0376567936, 1.0376273793, 1.0375980067,
    1.0375686757, 1.0375393862, 1.0375101381, 1.0374809313, 1.0374517657, 1.0374226413, 1.0373935580, 1.0373645156, 1.0373355141, 1.0373065535, 1.0372776336,
    1.0372487543, 1.0372199156, 1.0371911174, 1.0371623596, 1.0371336421, 1.0371049648, 1.0370763277, 1.0370477307, 1.0370191737, 1.0369906565, 1.0369621792,
    1.0369337417, 1.0369053438, 1.0368769856, 1.0368486668, 1.0368203874, 1.0367921475, 1.0367639467, 1.0367357852, 1.0367076628, 1.0366795795, 1.0366515350,
    1.0366235295, 1.0365955628, 1.0365676348, 1.0365397454, 1.0365118946, 1.0364840823, 1.0364563084, 1.0364285729, 1.0364008756, 1.0363732165, 1.0363455955,
    1.0363180125, 1.0362904675, 1.0362629603, 1.0362354910, 1.0362080594, 1.0361806655, 1.0361533091, 1.0361259902, 1.0360987088, 1.0360714647, 1.0360442579,
    1.0360170884, 1.0359899559, 1.0359628605, 1.0359358021, 1.0359087806, 1.0358817960, 1.0358548481, 1.0358279369, 1.0358010624, 1.0357742244, 1.0357474229,
    1.0357206578, 1.0356939290, 1.0356672365, 1.0356405802, 1.0356139600, 1.0355873758, 1.0355608277, 1.0355343154, 1.0355078390, 1.0354813984, 1.0354549934,
    1.0354286241, 1.0354022904, 1.0353759921, 1.0353497293, 1.0353235018, 1.0352973096, 1.0352711526, 1.0352450308, 1.0352189441, 1.0351928923, 1.0351668755,
    1.0351408936, 1.0351149465, 1.0350890341, 1.0350631564, 1.0350373133, 1.0350115047, 1.0349857306, 1.0349599909, 1.0349342856, 1.0349086145, 1.0348829776,
    1.0348573749, 1.0348318062, 1.0348062716, 1.0347807709, 1.0347553040, 1.0347298710, 1.0347044717, 1.0346791061, 1.0346537742, 1.0346284757, 1.0346032108,
    1.0345779793, 1.0345527811, 1.0345276163, 1.0345024847, 1.0344773862, 1.0344523209, 1.0344272885, 1.0344022892, 1.0343773228, 1.0343523893, 1.0343274885,
    1.0343026205, 1.0342777851, 1.0342529823, 1.0342282121, 1.0342034744, 1.0341787690, 1.0341540961, 1.0341294554, 1.0341048470, 1.0340802707, 1.0340557266,
    1.0340312145, 1.0340067344, 1.0339822862, 1.0339578699, 1.0339334854, 1.0339091326, 1.0338848116, 1.0338605222, 1.0338362643, 1.0338120380, 1.0337878431,
    1.0337636796, 1.0337395474, 1.0337154466, 1.0336913769, 1.0336673384, 1.0336433310, 1.0336193546, 1.0335954092, 1.0335714948, 1.0335476112, 1.0335237584,
    1.0334999364, 1.0334761451, 1.0334523844, 1.0334286543, 1.0334049547, 1.0333812855, 1.0333576468, 1.0333340385, 1.0333104604, 1.0332869126, 1.0332633950,
    1.0332399075, 1.0332164500, 1.0331930226, 1.0331696252, 1.0331462576, 1.0331229199, 1.0330996120, 1.0330763339, 1.0330530854, 1.0330298665, 1.0330066773,
    1.0329835175, 1.0329603872, 1.0329372864, 1.0329142149, 1.0328911727, 1.0328681597, 1.0328451759, 1.0328222213, 1.0327992958, 1.0327763993, 1.0327535318,
    1.0327306932, 1.0327078835, 1.0326851027, 1.0326623506, 1.0326396272, 1.0326169324, 1.0325942663, 1.0325716288, 1.0325490198, 1.0325264392, 1.0325038870,
    1.0324813632, 1.0324588677, 1.0324364004, 1.0324139613, 1.0323915504, 1.0323691676, 1.0323468128, 1.0323244861, 1.0323021872, 1.0322799163, 1.0322576732,
    1.0322354579, 1.0322132703, 1.0321911104, 1.0321689782, 1.0321468736, 1.0321247965, 1.0321027469, 1.0320807247, 1.0320587300, 1.0320367625, 1.0320148224,
    1.0319929095, 1.0319710239, 1.0319491653, 1.0319273339, 1.0319055295, 1.0318837521, 1.0318620017, 1.0318402782, 1.0318185815, 1.0317969117, 1.0317752686,
    1.0317536522, 1.0317320625, 1.0317104994, 1.0316889629, 1.0316674529, 1.0316459693, 1.0316245122, 1.0316030815, 1.0315816771, 1.0315602990, 1.0315389472,
    1.0315176215, 1.0314963220, 1.0314750486, 1.0314538013, 1.0314325799, 1.0314113845, 1.0313902151, 1.0313690715, 1.0313479537, 1.0313268617, 1.0313057955,
    1.0312847549, 1.0312637400, 1.0312427506, 1.0312217869, 1.0312008486, 1.0311799358, 1.0311590484, 1.0311381864, 1.0311173497, 1.0310965383, 1.0310757521,
    1.0310549912, 1.0310342554, 1.0310135446, 1.0309928590, 1.0309721984, 1.0309515627, 1.0309309520, 1.0309103662, 1.0308898052, 1.0308692690, 1.0308487576,
    1.0308282708, 1.0308078088, 1.0307873714, 1.0307669586, 1.0307465703, 1.0307262065, 1.0307058671, 1.0306855522, 1.0306652617, 1.0306449955, 1.0306247535,
    1.0306045358, 1.0305843424, 1.0305641730, 1.0305440278, 1.0305239067, 1.0305038096, 1.0304837365, 1.0304636874, 1.0304436622, 1.0304236608, 1.0304036833,
    1.0303837296, 1.0303637996, 1.0303438933, 1.0303240107, 1.0303041517, 1.0302843162, 1.0302645044, 1.0302447160, 1.0302249511, 1.0302052096, 1.0301854915,
    1.0301657968, 1.0301461253, 1.0301264771, 1.0301068521, 1.0300872503, 1.0300676717, 1.0300481161, 1.0300285837, 1.0300090742, 1.0299895877, 1.0299701242,
    1.0299506836, 1.0299312658, 1.0299118709, 1.0298924988, 1.0298731494, 1.0298538227, 1.0298345187, 1.0298152373, 1.0297959786, 1.0297767424, 1.0297575287,
    1.0297383375, 1.0297191687, 1.0297000224, 1.0296808984, 1.0296617967, 1.0296427173, 1.0296236602, 1.0296046253, 1.0295856126, 1.0295666220, 1.0295476536,
    1.0295287072, 1.0295097828, 1.0294908804, 1.0294720000, 1.0294531415, 1.0294343049, 1.0294154901, 1.0293966971, 1.0293779259, 1.0293591765, 1.0293404487,
    1.0293217426, 1.0293030582, 1.0292843953, 1.0292657540, 1.0292471342, 1.0292285358, 1.0292099590, 1.0291914035, 1.0291728694, 1.0291543567, 1.0291358652,
    1.0291173950, 1.0290989461, 1.0290805183, 1.0290621118, 1.0290437263, 1.0290253619, 1.0290070186, 1.0289886963, 1.0289703950, 1.0289521147, 1.0289338552,
    1.0289156167, 1.0288973990, 1.0288792021, 1.0288610260, 1.0288428706, 1.0288247360, 1.0288066220, 1.0287885287, 1.0287704559, 1.0287524038, 1.0287343722,
    1.0287163611, 1.0286983704, 1.0286804002, 1.0286624505, 1.0286445210, 1.0286266120, 1.0286087232, 1.0285908547, 1.0285730064, 1.0285551784, 1.0285373705,
    1.0285195827, 1.0285018151, 1.0284840675, 1.0284663400, 1.0284486325, 1.0284309449, 1.0284132773, 1.0283956296, 1.0283780018, 1.0283603938, 1.0283428057,
    1.0283252373, 1.0283076887, 1.0282901597, 1.0282726505, 1.0282551609, 1.0282376910, 1.0282202406, 1.0282028098, 1.0281853985, 1.0281680067, 1.0281506343,
    1.0281332814, 1.0281159479, 1.0280986338, 1.0280813389, 1.0280640634, 1.0280468072, 1.0280295702, 1.0280123524, 1.0279951538, 1.0279779744, 1.0279608140,
    1.0279436728, 1.0279265506, 1.0279094474, 1.0278923632, 1.0278752980, 1.0278582518, 1.0278412244, 1.0278242159, 1.0278072262, 1.0277902554, 1.0277733033,
    1.0277563700, 1.0277394554, 1.0277225595, 1.0277056823, 1.0276888237, 1.0276719837, 1.0276551623, 1.0276383594, 1.0276215750, 1.0276048091, 1.0275880617,
    1.0275713327, 1.0275546221, 1.0275379299, 1.0275212560, 1.0275046004, 1.0274879630, 1.0274713440, 1.0274547431, 1.0274381605, 1.0274215960, 1.0274050496,
    1.0273885214, 1.0273720112, 1.0273555191, 1.0273390450, 1.0273225889, 1.0273061507, 1.0272897305, 1.0272733282, 1.0272569438, 1.0272405772, 1.0272242284,
    1.0272078975, 1.0271915842, 1.0271752888, 1.0271590110, 1.0271427510, 1.0271265085, 1.0271102837, 1.0270940765, 1.0270778869, 1.0270617148, 1.0270455603,
    1.0270294232, 1.0270133036, 1.0269972014, 1.0269811166, 1.0269650492, 1.0269489992, 1.0269329664, 1.0269169510, 1.0269009529, 1.0268849719, 1.0268690082,
    1.0268530617, 1.0268371324, 1.0268212202, 1.0268053251, 1.0267894471, 1.0267735861, 1.0267577422, 1.0267419152, 1.0267261053, 1.0267103123, 1.0266945362,
    1.0266787770, 1.0266630347, 1.0266473093, 1.0266316006, 1.0266159088, 1.0266002337, 1.0265845753, 1.0265689337, 1.0265533088, 1.0265377005, 1.0265221089,
    1.0265065338, 1.0264909754, 1.0264754335, 1.0264599082, 1.0264443994, 1.0264289070, 1.0264134311, 1.0263979717, 1.0263825286, 1.0263671020, 1.0263516917,
    1.0263362977, 1.0263209200, 1.0263055587, 1.0262902135, 1.0262748847, 1.0262595720, 1.0262442755, 1.0262289952, 1.0262137310, 1.0261984829, 1.0261832509,
    1.0261680349, 1.0261528350, 1.0261376511, 1.0261224832, 1.0261073312, 1.0260921952, 1.0260770751, 1.0260619709, 1.0260468826, 1.0260318101, 1.0260167534,
    1.0260017125, 1.0259866873, 1.0259716780, 1.0259566843, 1.0259417064, 1.0259267441, 1.0259117974, 1.0258968664, 1.0258819510, 1.0258670512, 1.0258521669,
    1.0258372982, 1.0258224449, 1.0258076072, 1.0257927849, 1.0257779780, 1.0257631866, 1.0257484106, 1.0257336499, 1.0257189046, 1.0257041745, 1.0256894598,
    1.0256747604, 1.0256600762, 1.0256454073, 1.0256307535, 1.0256161150, 1.0256014916, 1.0255868833, 1.0255722902, 1.0255577122, 1.0255431492, 1.0255286012,
    1.0255140683, 1.0254995505, 1.0254850475, 1.0254705596, 1.0254560866, 1.0254416285, 1.0254271852, 1.0254127569, 1.0253983434, 1.0253839447, 1.0253695609,
    1.0253551918, 1.0253408374, 1.0253264978, 1.0253121730, 1.0252978628, 1.0252835673, 1.0252692864, 1.0252550202, 1.0252407686, 1.0252265315, 1.0252123090,
    1.0251981011, 1.0251839077, 1.0251697288, 1.0251555643, 1.0251414143, 1.0251272788, 1.0251131576, 1.0250990509, 1.0250849585, 1.0250708805, 1.0250568168,
    1.0250427674, 1.0250287323, 1.0250147115, 1.0250007049, 1.0249867125, 1.0249727343, 1.0249587703, 1.0249448205, 1.0249308848, 1.0249169632, 1.0249030557,
    1.0248891623, 1.0248752830, 1.0248614177, 1.0248475664, 1.0248337291, 1.0248199058, 1.0248060964, 1.0247923010, 1.0247785195, 1.0247647519, 1.0247509981,
    1.0247372582, 1.0247235321, 1.0247098199, 1.0246961214, 1.0246824367, 1.0246687658, 1.0246551086, 1.0246414651, 1.0246278353, 1.0246142191, 1.0246006167,
    1.0245870278, 1.0245734526, 1.0245598909, 1.0245463429, 1.0245328084, 1.0245192874, 1.0245057799, 1.0244922859, 1.0244788054, 1.0244653384, 1.0244518848,
    1.0244384446, 1.0244250178, 1.0244116044, 1.0243982043, 1.0243848176, 1.0243714442, 1.0243580841, 1.0243447373, 1.0243314038, 1.0243180835, 1.0243047764,
    1.0242914825, 1.0242782018, 1.0242649343, 1.0242516799, 1.0242384387, 1.0242252106, 1.0242119955, 1.0241987936, 1.0241856046, 1.0241724288, 1.0241592659,
    1.0241461161, 1.0241329792, 1.0241198553, 1.0241067443, 1.0240936462, 1.0240805611, 1.0240674889, 1.0240544295, 1.0240413829, 1.0240283492, 1.0240153283,
    1.0240023203, 1.0239893249, 1.0239763424, 1.0239633726, 1.0239504155, 1.0239374711, 1.0239245394, 1.0239116204, 1.0238987140, 1.0238858203, 1.0238729392,
    1.0238600706, 1.0238472147, 1.0238343713, 1.0238215405, 1.0238087221, 1.0237959163, 1.0237831230, 1.0237703422, 1.0237575738, 1.0237448179, 1.0237320743,
    1.0237193432, 1.0237066245, 1.0236939181, 1.0236812241, 1.0236685424, 1.0236558731, 1.0236432160, 1.0236305712, 1.0236179387, 1.0236053184, 1.0235927104,
    1.0235801145, 1.0235675309, 1.0235549594, 1.0235424001, 1.0235298530, 1.0235173180, 1.0235047951, 1.0234922842, 1.0234797855, 1.0234672988, 1.0234548242,
    1.0234423616, 1.0234299109, 1.0234174723, 1.0234050457, 1.0233926310, 1.0233802283, 1.0233678375, 1.0233554586, 1.0233430916, 1.0233307364, 1.0233183932,
    1.0233060617, 1.0232937421, 1.0232814343, 1.0232691383, 1.0232568541, 1.0232445816, 1.0232323209, 1.0232200719, 1.0232078347, 1.0231956091, 1.0231833952,
    1.0231711929, 1.0231590024, 1.0231468234, 1.0231346561, 1.0231225003, 1.0231103562, 1.0230982236, 1.0230861025, 1.0230739930, 1.0230618950, 1.0230498085,
    1.0230377335, 1.0230256700, 1.0230136180, 1.0230015773, 1.0229895481, 1.0229775303, 1.0229655239, 1.0229535289, 1.0229415453, 1.0229295729, 1.0229176120,
    1.0229056623, 1.0228937239, 1.0228817968, 1.0228698810, 1.0228579765, 1.0228460832, 1.0228342011, 1.0228223302, 1.0228104705, 1.0227986219, 1.0227867846,
    1.0227749584, 1.0227631433, 1.0227513393, 1.0227395464, 1.0227277647, 1.0227159940, 1.0227042343, 1.0226924857, 1.0226807481, 1.0226690215, 1.0226573059,
    1.0226456013, 1.0226339077, 1.0226222250, 1.0226105533, 1.0225988924, 1.0225872425, 1.0225756035, 1.0225639753, 1.0225523581, 1.0225407516, 1.0225291560,
    1.0225175712, 1.0225059972, 1.0224944340, 1.0224828816, 1.0224713399, 1.0224598090, 1.0224482888, 1.0224367794, 1.0224252806, 1.0224137925, 1.0224023151,
    1.0223908483, 1.0223793922, 1.0223679468, 1.0223565119, 1.0223450876, 1.0223336740, 1.0223222709, 1.0223108783, 1.0222994963, 1.0222881249, 1.0222767639,
    1.0222654135, 1.0222540735, 1.0222427440, 1.0222314250, 1.0222201164, 1.0222088183, 1.0221975306, 1.0221862533, 1.0221749863, 1.0221637298, 1.0221524836,
    1.0221412477, 1.0221300222, 1.0221188071, 1.0221076022, 1.0220964076, 1.0220852233, 1.0220740493, 1.0220628855, 1.0220517319, 1.0220405886, 1.0220294555,
    1.0220183326, 1.0220072199, 1.0219961174, 1.0219850250, 1.0219739427, 1.0219628706, 1.0219518086, 1.0219407567, 1.0219297149, 1.0219186832, 1.0219076615,
    1.0218966499, 1.0218856484, 1.0218746568, 1.0218636753, 1.0218527038, 1.0218417422, 1.0218307906, 1.0218198490, 1.0218089173, 1.0217979956, 1.0217870838,
    1.0217761819, 1.0217652899, 1.0217544077, 1.0217435354, 1.0217326730, 1.0217218204, 1.0217109777, 1.0217001448, 1.0216893216, 1.0216785083, 1.0216677047,
    1.0216569109, 1.0216461269, 1.0216353526, 1.0216245880, 1.0216138331, 1.0216030880, 1.0215923525, 1.0215816267, 1.0215709106, 1.0215602041, 1.0215495072,
    1.0215388200, 1.0215281424, 1.0215174743, 1.0215068159, 1.0214961671, 1.0214855278, 1.0214748980, 1.0214642778, 1.0214536672, 1.0214430660, 1.0214324743,
    1.0214218922, 1.0214113195, 1.0214007562, 1.0213902024, 1.0213796581, 1.0213691232, 1.0213585977, 1.0213480816, 1.0213375748, 1.0213270775, 1.0213165895,
    1.0213061109, 1.0212956417, 1.0212851817, 1.0212747311, 1.0212642898, 1.0212538577, 1.0212434350, 1.0212330215, 1.0212226173, 1.0212122223, 1.0212018366,
    1.0211914601, 1.0211810928, 1.0211707347, 1.0211603858, 1.0211500460, 1.0211397154, 1.0211293940, 1.0211190817, 1.0211087786, 1.0210984845, 1.0210881996,
    1.0210779237, 1.0210676570, 1.0210573993, 1.0210471506, 1.0210369110, 1.0210266805, 1.0210164589, 1.0210062464, 1.0209960429, 1.0209858484, 1.0209756628,
    1.0209654862, 1.0209553186, 1.0209451599, 1.0209350101, 1.0209248693, 1.0209147373, 1.0209046143, 1.0208945002, 1.0208843949, 1.0208742985, 1.0208642109,
    1.0208541322, 1.0208440623, 1.0208340012, 1.0208239490, 1.0208139055, 1.0208038708, 1.0207938449, 1.0207838278, 1.0207738194, 1.0207638197, 1.0207538288,
    1.0207438466, 1.0207338731, 1.0207239083, 1.0207139522, 1.0207040048, 1.0206940660, 1.0206841359, 1.0206742144, 1.0206643015, 1.0206543973, 1.0206445017,
    1.0206346147, 1.0206247362, 1.0206148664, 1.0206050051, 1.0205951524, 1.0205853082, 1.0205754725, 1.0205656454, 1.0205558268, 1.0205460166, 1.0205362150,
    1.0205264219, 1.0205166372, 1.0205068610, 1.0204970932, 1.0204873339, 1.0204775830, 1.0204678405, 1.0204581064, 1.0204483807, 1.0204386634, 1.0204289545,
    1.0204192539, 1.0204095617, 1.0203998778, 1.0203902023, 1.0203805351, 1.0203708762, 1.0203612256, 1.0203515833, 1.0203419493, 1.0203323235, 1.0203227061,
    1.0203130968, 1.0203034958, 1.0202939031, 1.0202843185, 1.0202747422, 1.0202651740, 1.0202556141, 1.0202460623, 1.0202365187, 1.0202269833, 1.0202174560,
    1.0202079368, 1.0201984258, 1.0201889229, 1.0201794281, 1.0201699414, 1.0201604628, 1.0201509923, 1.0201415298, 1.0201320754, 1.0201226291, 1.0201131907,
    1.0201037605, 1.0200943382, 1.0200849239, 1.0200755177, 1.0200661194, 1.0200567291, 1.0200473468, 1.0200379724, 1.0200286060, 1.0200192475, 1.0200098970,
    1.0200005544, 1.0199912197, 1.0199818929, 1.0199725739, 1.0199632629, 1.0199539598, 1.0199446645, 1.0199353770, 1.0199260974, 1.0199168257, 1.0199075617,
    1.0198983056, 1.0198890573, 1.0198798167, 1.0198705840, 1.0198613590, 1.0198521418, 1.0198429324, 1.0198337307, 1.0198245368, 1.0198153505, 1.0198061720,
    1.0197970013, 1.0197878382, 1.0197786828, 1.0197695351, 1.0197603950, 1.0197512626, 1.0197421379, 1.0197330208, 1.0197239114, 1.0197148096, 1.0197057154,
    1.0196966288, 1.0196875498, 1.0196784784, 1.0196694146, 1.0196603584, 1.0196513097, 1.0196422685, 1.0196332350, 1.0196242089, 1.0196151904, 1.0196061794,
    1.0195971759, 1.0195881799, 1.0195791914, 1.0195702103, 1.0195612368, 1.0195522707, 1.0195433120, 1.0195343608, 1.0195254171, 1.0195164807, 1.0195075518,
    1.0194986303, 1.0194897162, 1.0194808095, 1.0194719101, 1.0194630182, 1.0194541336, 1.0194452563, 1.0194363864, 1.0194275239, 1.0194186686, 1.0194098207,
    1.0194009801, 1.0193921468, 1.0193833208, 1.0193745021, 1.0193656907, 1.0193568865, 1.0193480896, 1.0193392999, 1.0193305175, 1.0193217423, 1.0193129744,
    1.0193042136, 1.0192954601, 1.0192867137, 1.0192779746, 1.0192692426, 1.0192605178, 1.0192518002, 1.0192430897, 1.0192343864, 1.0192256902, 1.0192170012,
    1.0192083193, 1.0191996444, 1.0191909767, 1.0191823161, 1.0191736626, 1.0191650161, 1.0191563768, 1.0191477445, 1.0191391192, 1.0191305010, 1.0191218898,
    1.0191132857, 1.0191046886, 1.0190960985, 1.0190875154, 1.0190789392, 1.0190703701, 1.0190618080, 1.0190532528, 1.0190447046, 1.0190361634, 1.0190276291,
    1.0190191017, 1.0190105813, 1.0190020678, 1.0189935612, 1.0189850615, 1.0189765688, 1.0189680829, 1.0189596039, 1.0189511317, 1.0189426665, 1.0189342080,
    1.0189257565, 1.0189173118, 1.0189088739, 1.0189004428, 1.0188920186, 1.0188836012, 1.0188751905, 1.0188667867, 1.0188583896, 1.0188499994, 1.0188416159,
    1.0188332391, 1.0188248691, 1.0188165059, 1.0188081494, 1.0187997996, 1.0187914566, 1.0187831203, 1.0187747907, 1.0187664677, 1.0187581515, 1.0187498420,
    1.0187415391, 1.0187332429, 1.0187249533, 1.0187166705, 1.0187083942, 1.0187001246, 1.0186918616, 1.0186836053, 1.0186753555, 1.0186671124, 1.0186588759,
    1.0186506459, 1.0186424226, 1.0186342058, 1.0186259956, 1.0186177919, 1.0186095948, 1.0186014043, 1.0185932202, 1.0185850428, 1.0185768718, 1.0185687073,
    1.0185605494, 1.0185523980, 1.0185442530, 1.0185361146, 1.0185279826, 1.0185198571, 1.0185117380, 1.0185036254, 1.0184955193, 1.0184874196, 1.0184793263,
    1.0184712394, 1.0184631590, 1.0184550850, 1.0184470174, 1.0184389562, 1.0184309013, 1.0184228529, 1.0184148108, 1.0184067751, 1.0183987458, 1.0183907228,
    1.0183827061, 1.0183746958, 1.0183666919, 1.0183586942, 1.0183507029, 1.0183427178, 1.0183347391, 1.0183267667, 1.0183188005, 1.0183108407, 1.0183028871,
    1.0182949397, 1.0182869987, 1.0182790639, 1.0182711353, 1.0182632129, 1.0182552968, 1.0182473870, 1.0182394833, 1.0182315858, 1.0182236946, 1.0182158095,
    1.0182079307, 1.0182000580, 1.0181921915, 1.0181843311, 1.0181764769, 1.0181686289, 1.0181607870, 1.0181529512, 1.0181451216, 1.0181372981, 1.0181294807,
    1.0181216695, 1.0181138643, 1.0181060652, 1.0180982723, 1.0180904854, 1.0180827046, 1.0180749298, 1.0180671611, 1.0180593985, 1.0180516419, 1.0180438914,
    1.0180361469, 1.0180284084, 1.0180206760, 1.0180129495, 1.0180052291, 1.0179975147, 1.0179898062, 1.0179821038, 1.0179744073, 1.0179667169, 1.0179590323,
    1.0179513538, 1.0179436812, 1.0179360145, 1.0179283538, 1.0179206990, 1.0179130502, 1.0179054072, 1.0178977702, 1.0178901391, 1.0178825139, 1.0178748946,
    1.0178672812, 1.0178596736, 1.0178520720, 1.0178444762, 1.0178368862, 1.0178293021, 1.0178217239, 1.0178141515, 1.0178065850, 1.0177990242, 1.0177914693,
    1.0177839202, 1.0177763770, 1.0177688395, 1.0177613078, 1.0177537819, 1.0177462618, 1.0177387475, 1.0177312389, 1.0177237361, 1.0177162391, 1.0177087478,
    1.0177012623, 1.0176937825, 1.0176863084, 1.0176788401, 1.0176713774, 1.0176639205, 1.0176564693, 1.0176490238, 1.0176415840, 1.0176341499, 1.0176267215,
    1.0176192987, 1.0176118816, 1.0176044702, 1.0175970644, 1.0175896643, 1.0175822698, 1.0175748810, 1.0175674978, 1.0175601202, 1.0175527482, 1.0175453819,
    1.0175380212, 1.0175306660, 1.0175233165, 1.0175159725, 1.0175086341, 1.0175013013, 1.0174939741, 1.0174866524, 1.0174793363, 1.0174720258, 1.0174647208,
    1.0174574213, 1.0174501274, 1.0174428390, 1.0174355561, 1.0174282787, 1.0174210068, 1.0174137405, 1.0174064796, 1.0173992242, 1.0173919743, 1.0173847299,
    1.0173774910, 1.0173702575, 1.0173630295, 1.0173558070, 1.0173485899, 1.0173413782, 1.0173341720, 1.0173269712, 1.0173197758, 1.0173125859, 1.0173054013,
    1.0172982222, 1.0172910485, 1.0172838802, 1.0172767172, 1.0172695597, 1.0172624075, 1.0172552607, 1.0172481192, 1.0172409832, 1.0172338524, 1.0172267271,
    1.0172196070, 1.0172124923, 1.0172053830, 1.0171982790, 1.0171911802, 1.0171840868, 1.0171769987, 1.0171699160, 1.0171628385, 1.0171557663, 1.0171486993,
    1.0171416377, 1.0171345813, 1.0171275303, 1.0171204844, 1.0171134439, 1.0171064085, 1.0170993785, 1.0170923536, 1.0170853340, 1.0170783196, 1.0170713105,
    1.0170643066, 1.0170573078, 1.0170503143, 1.0170433260, 1.0170363429, 1.0170293650, 1.0170223922, 1.0170154246, 1.0170084622, 1.0170015050, 1.0169945530,
    1.0169876060, 1.0169806643, 1.0169737277, 1.0169667962, 1.0169598699, 1.0169529486, 1.0169460325, 1.0169391216, 1.0169322157, 1.0169253150, 1.0169184193,
    1.0169115287, 1.0169046433, 1.0168977629, 1.0168908876, 1.0168840174, 1.0168771522, 1.0168702921, 1.0168634371, 1.0168565871, 1.0168497421, 1.0168429022,
    1.0168360674, 1.0168292375, 1.0168224127, 1.0168155929, 1.0168087781, 1.0168019684, 1.0167951636, 1.0167883638, 1.0167815691, 1.0167747793, 1.0167679945,
    1.0167612147, 1.0167544398, 1.0167476699, 1.0167409050, 1.0167341450, 1.0167273900, 1.0167206400, 1.0167138948, 1.0167071546, 1.0167004194, 1.0166936890,
    1.0166869636, 1.0166802431, 1.0166735275, 1.0166668168, 1.0166601110, 1.0166534101, 1.0166467141, 1.0166400230, 1.0166333367, 1.0166266553, 1.0166199788,
    1.0166133072, 1.0166066404, 1.0165999784, 1.0165933214, 1.0165866691, 1.0165800217, 1.0165733791, 1.0165667414, 1.0165601084, 1.0165534803, 1.0165468570,
    1.0165402385, 1.0165336248, 1.0165270159, 1.0165204118, 1.0165138125, 1.0165072179, 1.0165006282, 1.0164940432, 1.0164874630, 1.0164808875, 1.0164743168,
    1.0164677508, 1.0164611896, 1.0164546332, 1.0164480814, 1.0164415344, 1.0164349921, 1.0164284546, 1.0164219218, 1.0164153936, 1.0164088702, 1.0164023515,
    1.0163958375, 1.0163893282, 1.0163828235, 1.0163763236, 1.0163698283, 1.0163633377, 1.0163568517, 1.0163503705, 1.0163438938, 1.0163374219, 1.0163309546,
    1.0163244919, 1.0163180339, 1.0163115805, 1.0163051317, 1.0162986875, 1.0162922480, 1.0162858131, 1.0162793828, 1.0162729571, 1.0162665360, 1.0162601195,
    1.0162537076, 1.0162473002, 1.0162408975, 1.0162344993, 1.0162281057, 1.0162217167, 1.0162153322, 1.0162089523, 1.0162025770, 1.0161962062, 1.0161898399,
    1.0161834782, 1.0161771210, 1.0161707683, 1.0161644202, 1.0161580765, 1.0161517374, 1.0161454029, 1.0161390728, 1.0161327472, 1.0161264261, 1.0161201095,
    1.0161137974, 1.0161074898, 1.0161011866, 1.0160948879, 1.0160885937, 1.0160823040, 1.0160760187, 1.0160697379, 1.0160634615, 1.0160571896, 1.0160509221,
    1.0160446590, 1.0160384004, 1.0160321462, 1.0160258965, 1.0160196511, 1.0160134102, 1.0160071737, 1.0160009415, 1.0159947138, 1.0159884905, 1.0159822716,
    1.0159760570, 1.0159698469, 1.0159636411, 1.0159574397, 1.0159512427, 1.0159450500, 1.0159388617, 1.0159326777, 1.0159264981, 1.0159203229, 1.0159141520,
    1.0159079854, 1.0159018231, 1.0158956652, 1.0158895116, 1.0158833624, 1.0158772174, 1.0158710768, 1.0158649405, 1.0158588084, 1.0158526807, 1.0158465573,
    1.0158404381, 1.0158343233, 1.0158282127, 1.0158221064, 1.0158160044, 1.0158099066, 1.0158038131, 1.0157977239, 1.0157916389, 1.0157855582, 1.0157794817,
    1.0157734095, 1.0157673415, 1.0157612777, 1.0157552182, 1.0157491628, 1.0157431118, 1.0157370649, 1.0157310222, 1.0157249838, 1.0157189495, 1.0157129195,
    1.0157068936, 1.0157008720, 1.0156948545, 1.0156888412, 1.0156828321, 1.0156768272, 1.0156708264, 1.0156648298, 1.0156588373, 1.0156528491, 1.0156468649,
    1.0156408850, 1.0156349091, 1.0156289374, 1.0156229699, 1.0156170065, 1.0156110472, 1.0156050920, 1.0155991410, 1.0155931940, 1.0155872512, 1.0155813125,
    1.0155753779, 1.0155694474, 1.0155635210, 1.0155575986, 1.0155516804, 1.0155457663, 1.0155398562, 1.0155339502, 1.0155280483, 1.0155221504, 1.0155162566,
    1.0155103669, 1.0155044812, 1.0154985996, 1.0154927220, 1.0154868485, 1.0154809790, 1.0154751135, 1.0154692521, 1.0154633946, 1.0154575413, 1.0154516919,
    1.0154458465, 1.0154400052, 1.0154341679, 1.0154283345, 1.0154225052, 1.0154166799, 1.0154108585, 1.0154050412, 1.0153992278, 1.0153934184, 1.0153876130,
    1.0153818115, 1.0153760140, 1.0153702205, 1.0153644309, 1.0153586453, 1.0153528637, 1.0153470860, 1.0153413122, 1.0153355424, 1.0153297765, 1.0153240146,
    1.0153182565, 1.0153125024, 1.0153067522, 1.0153010060, 1.0152952636, 1.0152895252, 1.0152837906, 1.0152780600, 1.0152723333, 1.0152666104, 1.0152608914,
    1.0152551764, 1.0152494652, 1.0152437579, 1.0152380544, 1.0152323549, 1.0152266592, 1.0152209673, 1.0152152793, 1.0152095952, 1.0152039150, 1.0151982385,
    1.0151925659, 1.0151868972, 1.0151812323, 1.0151755712, 1.0151699140, 1.0151642606, 1.0151586110, 1.0151529652, 1.0151473232, 1.0151416851, 1.0151360507,
    1.0151304202, 1.0151247934, 1.0151191705, 1.0151135513, 1.0151079360, 1.0151023244, 1.0150967166, 1.0150911125, 1.0150855123, 1.0150799158, 1.0150743231,
    1.0150687341, 1.0150631489, 1.0150575675, 1.0150519898, 1.0150464158, 1.0150408456, 1.0150352792, 1.0150297164, 1.0150241574, 1.0150186022, 1.0150130506,
    1.0150075028, 1.0150019587, 1.0149964183, 1.0149908817, 1.0149853487, 1.0149798194, 1.0149742939, 1.0149687720, 1.0149632539, 1.0149577394, 1.0149522286,
    1.0149467215, 1.0149412180, 1.0149357183, 1.0149302222, 1.0149247298, 1.0149192410, 1.0149137559, 1.0149082745, 1.0149027967, 1.0148973226, 1.0148918521,
    1.0148863853, 1.0148809221, 1.0148754625, 1.0148700066, 1.0148645543, 1.0148591056, 1.0148536606, 1.0148482191, 1.0148427813, 1.0148373471, 1.0148319165,
    1.0148264895, 1.0148210662, 1.0148156464, 1.0148102302, 1.0148048176 };

/********************************************************************************************************************************************************//**
 * \brief New fast lookup table version of the old kgw algo.
 *
 *        Attributes include, but are not limited to:
 *          No longer need CBigNum from your old Openssl based code in order to work.
 *             The uint256 is used for the difficulty calculations, and the code insures NO negative value is ever produced.
 *          Strictly unsigned integer math, except for a tiny bit of floating point left in the algorithm.
 *             During development it was found that all 10 digits of precision were required in our new array of 'double' floating
 *             point values, you find above. To load our 280K+ blocks from Anoncoin's past mined with KGW, and do it without error,
 *             6 digits were not enough, so it is now set to the limit of 'double' (typically 10) on most machines found today. for
 *             that very specific reason.
 *        All variables are spelled out as to the size and signed/unsigned quantities being referenced.  This code should compile
 *             and run on any type of processor, and work correctly.
 ***
 *        This code is totally v10 core technology compatible....
 ***
 *        The algorithm is very sensitive to even the smallest calculation error, and took some considerable effort to develop
 *        and debug...
 *        If it helps you, please consider donating your favorite coin to my GR development fund. Email me: groundrod@anoncoin.net
 *        for a current list of send to addresses, or just to let me know it worked for you as well...GR
 *
 * \param pindexLast const CBlockIndex*
 * \return the calculated new difficulty result
 *
 ***********************************************************************************************************************************************************/

static int64_t abs64(int64_t n)
{
    return (n >= 0 ? n : -n);
}

static arith_uint256 NextWorkRequiredKgwV2(const CBlockIndex* pindexLast, const Consensus::Params& params)
{
    uint32_t nActualRateSecs = 0;
    uint32_t nTargetRateSecs = 0;
    int32_t  nBlockMass = 1;
    int64_t  nLastBlockSolvedTime = pindexLast->GetBlockTime();
    double   dRateAdjustmentRatio;
    arith_uint256  uintDifficultyAvg = ~arith_uint256(0);                 //! If the loop can't even start, this ensures the min pow gets returned.
    const CBlockIndex *pBlockReading = pindexLast;                  //! Start out with the last known block index

    //! A minimum of .25 days worth of blocks needs to be available before the Kgw algo can execute
    if( pindexLast->nHeight >= nMinBlocksToAvg )
        for( ; nBlockMass <= nMaxBlocksToAvg && pBlockReading && pBlockReading->nHeight > 0; nBlockMass++, pBlockReading = pBlockReading->pprev ) {
            arith_uint256  uintNextSample;
            uintNextSample.SetCompact(pBlockReading->nBits);                            //! Setup a full 256bit unsigned integer of the next block's compact difficulty
            if( nBlockMass == 1 )
                uintDifficultyAvg = uintNextSample;                                     //! The most recent sample is our initial difficulty
            else {
                bool fSign = uintNextSample < uintDifficultyAvg;                        //! Keep the value positive, after all, we're working with unsigned big integers
                arith_uint256  uintSampleDiff;
                uintSampleDiff = fSign ? uintDifficultyAvg - uintNextSample : uintNextSample - uintDifficultyAvg;
                uintSampleDiff /= (uint32_t)nBlockMass;                                 //! Diminishing effect on the difficulty adjustment as the samples get older
                if( fSign ) uintDifficultyAvg -= uintSampleDiff; else uintDifficultyAvg += uintSampleDiff;
                //! At this point uintDifficultyAvg is a positive value, with a diminishing part of the next older samples having been +/- to the new value
            }
            //! Now that difficulty has been updated, we turn to the issue of evaluating the time aspect of our blockchain.
            nActualRateSecs = (uint32_t)(abs64(nLastBlockSolvedTime - pBlockReading->GetBlockTime()));  //! Keep the times positive as well & update it as we go back in time
            /**
             * Critical position here on the next line of code, and its not in the right place, but is totally required or old Anoncoin blocks
             * will not validate.
             * This must appear BEFORE the ratio is calculated and BEFORE exiting the loop to process the final difficulty value, so it works the same as has been
             * done in past versions of this software.
             * Technically, to be correct this should appear near the end of the loop to update the target rate AFTER those steps, so when nBlockMass = 2, it will
             * finally have the <1st> interval set to 1 nTargetRateSecs correctly.  Bad programming from the past has been kept compatible here, new development or
             * hardforks should consider this as one of the required design changes.  This section of code should not even execute until AFTER information on 2
             * blocks has been gathered, I've started one possible part of a solution already on nActualRateSecs for you below, its always 0 on the 1st pass...GR
             */
            nTargetRateSecs += (uint32_t)nTargetSpacing;
            dRateAdjustmentRatio = nActualRateSecs ? double(nTargetRateSecs) / double(nActualRateSecs) : double(1);
            double   dEventHorizonFast;
            double   dEventHorizonSlow;
            //! v2 implements the much faster version of this line: dEventHorizonFast = 1.0 + (0.7084 * pow((double(nBlockMass)/double(144.0)), -1.228));
            dEventHorizonFast = kgw_blockmass_curve[ nBlockMass - 1 ];
            dEventHorizonSlow = 1.0 / dEventHorizonFast;
            //! All it is ever used for is to decide when to exit this loop
            if( nBlockMass >= nMinBlocksToAvg && ( dRateAdjustmentRatio <= dEventHorizonSlow || dRateAdjustmentRatio >= dEventHorizonFast ) )
                break;
            if (pBlockReading->pprev == NULL)
                break;
        }
    //! Create a new variable, really only needed for convenience and logging
    arith_uint256 uintNewDifficulty = uintDifficultyAvg;                                          //! No need to verify our value is positive at this point
    if (nActualRateSecs != 0 && nTargetRateSecs != 0) {                                     //! Could be our time rates haven't been updated though, be sure and check
        uintNewDifficulty *= nActualRateSecs;                                               //! Apply 1 / <adj ratio> using unsigned big integer math
        uintNewDifficulty /= nTargetRateSecs;                                               //! This ties the new difficulty to how far off the time target we are.
    }
    // debug print
#if defined( LOG_DEBUG_OUTPUT )
    LogPrintf("Difficulty Retarget - Kimoto Gravity Well v2.0\n");
    LogPrintf("  Before: %08x %s\n", pindexLast->nBits, arith_uint256().SetCompact(pindexLast->nBits).ToString());
    LogPrintf("  After : %08x %s\n", uintNewDifficulty.GetCompact(), uintNewDifficulty.ToString());
#endif
    const arith_uint256 &uintPOWlimit = UintToArith256(params.powLimit);
    if( uintNewDifficulty > uintPOWlimit ) {
        LogPrintf("Block at Height %d, Computed Next Work Required %0x limited and set to Minimum %0x\n", pindexLast->nHeight, uintNewDifficulty.GetCompact(), uintPOWlimit.GetCompact());
        uintNewDifficulty = uintPOWlimit;
    }
    return uintNewDifficulty;
}

CRetargetPidController::CRetargetPidController( const double dProportionalGainIn, const int64_t nIntegrationTimeIn, const double dIntegratorGainIn, const double dDerivativeGainIn, const Consensus::Params& params ) :
    dProportionalGain(dProportionalGainIn), nIntegrationTime(nIntegrationTimeIn), dIntegratorGain(dIntegratorGainIn), dDerivativeGain(dDerivativeGainIn)
{
    fTipFilterInitialized = false;
    nIntegratorHeight = nIndexFilterHeight = 0;
    nLastCalculationTime = 0;
    nBlocksSampled = 0;
    uintTestNetStartingDifficulty = UintToArith256(params.powLimit);
#if defined( HARDFORK_BLOCK )
    if( Params().isMainNetwork() ) {
        nTipFilterBlocks = atoi( TIPFILTERBLOCKS_DEFAULT );
        fUsesHeader = USESHEADER_DEFAULT;
    } else {
        nTipFilterBlocks = atoi( gArgs.GetArg("-retargetpid.tipfilterblocks", TIPFILTERBLOCKS_DEFAULT ).c_str() );
        if( nTipFilterBlocks < 5 ) nTipFilterBlocks = 5;
        fUsesHeader = gArgs.GetBoolArg( "-retargetpid.useheader", USESHEADER_DEFAULT );
    }
#else
    // Before the hardfork build, we allow programmable settings on both mainnet and testnets
    nTipFilterBlocks = atoi( gArgs.GetArg("-retargetpid.tipfilterblocks", TIPFILTERBLOCKS_DEFAULT ).c_str() );
    if( nTipFilterBlocks < 5 ) nTipFilterBlocks = 5;
    fUsesHeader = gArgs.GetBoolArg( "-retargetpid.useheader", USESHEADER_DEFAULT );
#endif
    //! This is only an issue for TestNets...
    //! The starting difficulty value provided is the number of times greater than the minimum difficulty, so in order
    //! to calculate the block difficulty, we take the user selected value and simply divide the minimum difficulty by
    //! that amount.
    if( !Params().isMainNetwork() ) {
        int32_t nConfigStartingDifficulty;
        nConfigStartingDifficulty = roundint( atof( gArgs.GetArg("-retargetpid.startingdiff", "1.0" ).c_str() ) );
        if( nConfigStartingDifficulty < 1 ) nConfigStartingDifficulty = 1;
        uintTestNetStartingDifficulty /= (uint32_t)nConfigStartingDifficulty;
    }

    //! (Re)generate CSV report headers
    fRetargetNewLog = true;
    fDiffCurvesNewLog = true;
    fLogDiffLimits = gArgs.GetBoolArg( "-retargetpid.logdifflimits", true );

    if( Params().isMainNetwork() ) {
        nMaxDiffIncrease = atoi( NMAXDIFFINCREASE );
        nMaxDiffDecrease = atoi( NMAXDIFFDECREASE );
    } else {

    int32_t nDifficulty;
    //! Difficulty is harder as the 256bit number goes down.  the Previous Difficulty is divided by maxdiffincrease, and compared
    //! to the calculated value, if it is less than that value, the output is set to it as the maximum change.
    nDifficulty = atoi( gArgs.GetArg("-retargetpid.maxdiffincrease", "101" ).c_str() );
    if( nDifficulty < 101 ) nDifficulty = 101;
    nMaxDiffIncrease = (uint32_t)nDifficulty;

    //! maxdiffdecrease is a divider upon the prevDifficulty
    //! Difficulty is easier as the 256bit number goes up.  the Previous Difficulty is multiplied by maxdiffdecrease, and compared
    //! to the calculated value, if it is more than that value, the output is set to it as the maximum change.
    nDifficulty = atoi( gArgs.GetArg("-retargetpid.maxdiffdecrease", "101" ).c_str() );
    if( nDifficulty < 101 ) nDifficulty = 101;
    nMaxDiffDecrease = (uint32_t)nDifficulty;
    }
}

//! As the retargetpid data is all private we must have public routines to access various values, this one gets the terms
void CRetargetPidController::GetPidTerms( double* pProportionalGainOut, int64_t* pIntegratorTimeOut, double* pIntegratorGainOut, double* pDerivativeGainOut )
{
    *pProportionalGainOut = dProportionalGain;
    *pIntegratorTimeOut = nIntegrationTime;
    *pIntegratorGainOut = dIntegratorGain;
    *pDerivativeGainOut = dDerivativeGain;
}

//! Class method definitions for the CRetargetPidController
bool CRetargetPidController::IsPidUpdateRequired( const CBlockIndex* pIndex, const CBlockHeader* pBlockHeader )
{
    assert( pIndex && pBlockHeader );
    return nIntegratorHeight != pIndex->nHeight || nLastCalculationTime != pBlockHeader->GetBlockTime() || nIndexFilterHeight != pIndex->nHeight;
}

//! Return the output result, used internally while LOCK is held.
arith_uint256 CRetargetPidController::GetRetargetOutput()
{
    return uintTargetAfterLimits;
}

//! Returns the number of seconds per block that the current chain is operating at.
//! This result spans the oldest to newest blockindex integration period, and is returned as an int64_t value rounded
//! up/down from the calculated double blocktime to the nearest second
double CRetargetPidController::GetIntegrationBlockTime( void )
{
    return dIntegratorBlockTime;
}

//! Returns the number of blocks used by the Tip Filter
int32_t CRetargetPidController::GetTipFilterBlocks()
{
    return nTipFilterBlocks;
}

//! Returns true if this retargetpid uses headers in the output difficulty calculations
bool CRetargetPidController::UsesHeader()
{
    return fUsesHeader;
}

//! Returns the testnet difficulty used to initialize the chain
arith_uint256 CRetargetPidController::GetTestNetStartingDifficulty( void )
{
    return uintTestNetStartingDifficulty;
}

bool CRetargetPidController::LimitOutputDifficultyChange( arith_uint256& uintResult, const arith_uint256& uintCalculated, const arith_uint256& uintPOWlimit, const CBlockIndex* pIndex )
{
    const int64_t nLastBlockIndexTime = pIndex->GetBlockTime();
    const int64_t nTimeSinceLastBlock = nLastCalculationTime - nLastBlockIndexTime;
    pIndex = pIndex->pprev; 
    const int64_t nPreviousBlockIndexTime = pIndex->GetBlockTime();
    pIndex = pIndex->pprev;
    const int64_t nBeforePreviousBlockIndexTime = pIndex->GetBlockTime();
    const int64_t nLastBlockSpace = nLastBlockIndexTime - nPreviousBlockIndexTime;
    const int64_t nLast2BlockSpace = nLastBlockIndexTime - nBeforePreviousBlockIndexTime;
    const uint32_t nIntervalForceDiffDecrease = 3 * nTargetSpacing;
    const uint32_t nInterval2ForceDiffDecrease = 5 * nTargetSpacing;
    const uint32_t nIntervalForceExtDiffDecrease = 10 * nTargetSpacing;
    bool fLimited = true;                                       //! Assume limit need to be applied to the result.
    


    if( uintCalculated < uintPrevDiffForLimitsLast ) {          // CSlave: If the new diff < previous diff of last block, assume an increase of diff.
        if( uintCalculated < uintDiffAtMaxIncreaseTip ) {       // Check the DiffAtMaxIncrease that is calculated on the partial tip to cap the increase of Diff. 
            uintResult = uintDiffAtMaxIncreaseTip;              // Set the result equal to the maximum difficulty increase limit. A smaller number is more difficult.
            if( nLastBlockSpace >= nIntervalForceDiffDecrease)  // Check if the previous block space length is greater than nIntervalForceDiffDecrease.      
                uintResult = uintDiffAtMaxDecreaseLast;         // If so, decrease the difficulty to the max decrease value calculated from the last block.
        } else {                                                // If the Diff calculated did not hit the upper moving average from the partial tip UP...
           uintResult = uintCalculated;                         // Give the Diff value the calculated value from the TipFilter for not-caped block.
           fLimited = false; 
           if( nLastBlockSpace >= nIntervalForceDiffDecrease) { // Check if the previous block space length is greater than nIntervalForceDiffDecrease.
               fLimited = true; 
               uintResult = uintDiffAtMaxDecreaseTip;           // Decrease the Diff to the value calculated on the moving average from the partial tip DOWN.
               if( uintResult > uintDiffAtMaxDecreaseLast)      // Is the Diff we set easier than the Diff decrease we get by calculating on the last block?
                   uintResult = uintDiffAtMaxDecreaseLast;      // Then set the value to the one calculated on the last block, which is at a higher difficulty.
           }
        }
  
    } else {                                                    // CSlave: If the new diff > previous diff of last block, assume a decrease of diff.
        if( uintCalculated > uintDiffAtMaxDecreaseLast && nLastBlockSpace < nIntervalForceDiffDecrease && nLast2BlockSpace < nInterval2ForceDiffDecrease) { // Check the 2 previous block space length and cap the decrease of Diff.
            uintResult = uintDiffAtMaxDecreaseLast;             // Set the result equal to the maximum difficulty decrease limit for subsequent block.
            if( uintResult > uintDiffAtMaxDecreaseTip && nLastBlockSpace < nTargetSpacing ) // Did the difficulty decrease went lower than the value calculated from the partial tip? Was the previous block quick?
                uintResult = uintDiffAtMaxDecreaseTip;          // Yes, cap it at the value calculated from the partial tip average. If the previous block was slow then keep the lower diff! 
            //! There was asked for checking the block space: when the block difficulty is below the tip avg DOWN, we keep the avg limit if the previous block was quick, but if it was slow we let the difficulty drop below the avg.
            //! This ensure a more rapid decrease of Diff in the case of huge hash drop, but as soon as the blocks become quick again the difficulty jump back up to the average calculated from the partial tip DOWN.
        } else {                                                // Did not hit the difficulty limit calculated from the last block, but have to check for the one calculated from the tip, in case the tip is near. 
            if( uintCalculated > uintDiffAtMaxDecreaseTip && nLastBlockSpace < nTargetSpacing)     // Is the decreased difficulty lower than the value calculated from the partial tip and was the previous block quick?
                uintResult = uintDiffAtMaxDecreaseTip;          // If below the avg of the partial tip DOWN, and the previous block was quick, let it go up to the partial tip difficulty avg.
            else {       // Several possibilities: the difficulty is capped neither by the MaxDecreaseLast or MaxDecreaseTip or else the previous block space lengths are >= nIntervalForceDiffDecrease or nInterval2ForceDiffDecrease.
                if( nLastBlockSpace >= nIntervalForceDiffDecrease || nLast2BlockSpace >= nInterval2ForceDiffDecrease) // In case of a very slow block, it will activate twice due to nInterval2ForceDiffDecrease. 
                    uintResult = uintDiffAtMaxDecreaseLast;              
                else {
                uintResult = uintCalculated;                    // Give the Diff value the calculated value from the TipFilter for not-caped block.
                fLimited = false; }
            }
        }
    } 

    if (uintResult < uintDiffAtMaxDecreaseTip && nTimeSinceLastBlock >= nIntervalForceExtDiffDecrease && pIndex->nHeight > HARDFORK_BLOCK2) { 
        uintResult = uintDiffAtMaxDecreaseTip;
		fLimited = true; }

    //! Lastly a check is made to see that the difficulty is not less than the absolute limit, this is also done it NextWorkRequired, but we need it
    //! done here too for TestNets and diagnostic logging.
    if( uintResult > uintPOWlimit ) {
        uintResult = uintPOWlimit;
        fLimited = true;
    }

    return fLimited;
}

bool CRetargetPidController::UpdateFilterTimingResults( std::vector<FilterPoint>& vFilterPoints )
{
    uint32_t nDividerSum = 0;
    uint32_t nBlockSpacingSum = 0;
    int64_t nBlockSpacing, nTimeError, nTimeError0, nChangeRate;

    //! If the header is included, the size of the filter is one larger than the TipFilterBlocks,
    //! by using the given filters size, instead of that constant, this code works either way.
    int32_t nFilterSize = vFilterPoints.size();
    //! Initialize the results, we are about to calculate new values for.  Note also that the
    //! total weight values will be used and updated.
    dSpacingError = 0.0;
    dRateOfChange = 0.0;
    for( int32_t i = 1; i <= nFilterSize; i++ ) {
        //! Process the time values
        if( i < nFilterSize ) {
            nBlockSpacing = vFilterPoints[i].nBlockTime - vFilterPoints[i - 1].nBlockTime;
            vFilterPoints[i].nSpacing = (int32_t)nBlockSpacing;
            nBlockSpacingSum += (uint32_t)nBlockSpacing;
            nTimeError = nBlockSpacing - nTargetSpacing;
            vFilterPoints[i].nSpacingError = (int32_t)nTimeError;
            if( i > 1 ) {
                nChangeRate = nTimeError - nTimeError0;
                vFilterPoints[i].nRateOfChange = (int32_t)nChangeRate;
                dRateOfChange += (double)( (int32_t)nChangeRate * (i - 1) );
            }
            nTimeError0 = nTimeError;
            dSpacingError += (double)( (int32_t)nTimeError * i );
            nDividerSum += i;
        } else {
            dAverageTipSpacing = (double)nBlockSpacingSum / (double)(i - 1);
            nSpacingErrorWeight = nDividerSum;
            dSpacingError /= (double)nDividerSum;
            nRateChangeWeight = nDividerSum - i + 1;
            dRateOfChange /= (double)nRateChangeWeight;
        }
    }
}

//! Anoncoin retarget system can consider the case of the next new block which has not yet
//! been mined.  The option is available for both the P and D terms.  This can not be enabled,
//! unless a precise control of peer clocks is considered as well, no block times from the
//! future can be allowed, if the header is included in retargeting difficulty.  If used,
//! the spacing error here can be positive or negative, unlike the tip filter, where all
//! times are first sorted.
//! Both the spacing error and rate of change use the most recent block time from the filter
//! as reference, to create the header values, then if enabled they carry equal weight to
//! all the past error, not just more important as the tip filter handles newer block times
//! from the index.  The header spacing error, and thus by reference the rate of change is
//! equal in importance to ALL the past error.

//! If the header (nTipTime) is not used by this controller, the Tip Filter already has the
//! results we need calculated from past block values, and so this routine does little more
//! than assign the results to the parameter references specified by the caller.
//!
//! So with all of that in mind, let us move on to considering our source of samples, how
//! the rest of the software works and see if we can come up with a calculation that is fast,
//! smart and deals well with reporting block time error at the tip...
//! NOTE: ToDo: external miners wanting access to the above difficulty adjustment calculation.
bool CRetargetPidController::CalcBlockTimeErrors( const int64_t nTipTime )
{
    bool fErrorCalculated = true;                                   //! Assume success

    if( fTipFilterInitialized ) {
        //! If the header is used, the block time spacing error and rate of change must be calculated now.
        if( fUsesHeader ) {
            bool fHeaderAdded = false;
            FilterPoint aHeaderPoint;
            //! Build a blank Tip Filter data point with the TipTime set.
            aHeaderPoint.nBlockTime = nTipTime;
            //! The header point can be identified, because it is the only one for which difficulty is infinite (0)
            aHeaderPoint.nDiffBits = 0;
            aHeaderPoint.nSpacing = aHeaderPoint.nSpacingError = aHeaderPoint.nRateOfChange = 0;
            //! Build the finalized tip filter, inserting the header block at the correctly sorted time position
            vTipFilterWithHeader.clear();
            //! We need to keep the oldest index entry, as it is used to calculate the previous difficulty, or i could start at 1
            for( int32_t i = 0; i < nTipFilterBlocks; i++ ) {
                if( !fHeaderAdded && nTipTime < vIndexTipFilter[i].nBlockTime ) {
                    vTipFilterWithHeader.push_back( aHeaderPoint );
                    fHeaderAdded = true;
                }
                vTipFilterWithHeader.push_back( vIndexTipFilter[i] );
            }
            //! If the new tiptime header was not yet added, it must be newer than all previous block times, so place it at the end of the filter
            if( !fHeaderAdded )
                vTipFilterWithHeader.push_back( aHeaderPoint );

            assert( vTipFilterWithHeader.size() == nTipFilterBlocks + 1 );
            UpdateFilterTimingResults( vTipFilterWithHeader );
        }
        //! else we already have the calculations done, when the index tip filter was initialized, no further processing is needed.

        //! At this point we have a one, or if need be two sorted tip filters. The samples have been weighted and used to calculate
        //! spacing error and rate of change. The previous difficulty was always only calculated from the index blocks and no added
        //! information can be gleaned from a header.
        //!
        //! If header information is to be used, it is important to understand why all the above code and extra filter needed to be
        //! setup and used.  The nTipTime may NOT have been > the the last block time in the index, in fact it could be much less,
        //! and the spacing error very negative, that is why the above process is done, to ensure the times remain and are processed
        //! in a sorted order.  Also if a negative time header was being considered, its weight important will be far less than if it
        //! had been the most recent and newer than any other block time, normally this is what will happen, and the header time will
        //! be considered the most important weighted time in spacing and rate of change calculations.
        //! By having this technology available as a separate method, it is also easy to run difficulty curves and project new retarget
        //! outputs, without allot of overhead and unnecessary calculations being done.
    } else
        fErrorCalculated = false;

    return fErrorCalculated;
}

//! Updates the TipFilter based on on the BlockIndex, used to calculate instantaneous block spacing, rate of changes & limits.
bool CRetargetPidController::UpdateIndexTipFilter( const CBlockIndex* pIndex )
{
    //! We use the given block index height as our starting point, but we must make sure we have enough
    //! blocks, and that it does not include the genesis block's time, that is very old and should not be used.
    if( pIndex->nHeight < nTipFilterBlocks )
        return false;

    //! This matters if fUsesHeader has been turned on.
    //! Make sure to force a new block spacing error calculation next time GetNextWorkRequired is run.
    //! Otherwise what could happen is the most recent results may only 'appear' to have been set correctly.
    //! When in fact, they are not for this BlockIndex height, and go undetected by the IsPidUpdateRequired()
    nLastCalculationTime = 0;

    //! Unless some task is requesting the filter to re-initialized, if this is being called for the exact same
    //! height as our previous calculation, then we are done.
    if( fTipFilterInitialized && nIndexFilterHeight == pIndex->nHeight )
        return true;

    //! Initialize the Tip Filter data points based on the block index values
    // To aid in debugging problems, it maybe useful to clear the block timing results involved in these calculations.
    nSpacingErrorWeight = nRateChangeWeight = 0;
    dAverageTipSpacing = dSpacingError = dRateOfChange = 0.0;
    vIndexTipFilter.clear();

    // bool fDiffPrevFromHash = GetBoolArg( "-retargetpid.diffprevfromhash", false );
    FilterPoint aFilterPoint;
    const CBlockIndex* pIndexSearch = pIndex;
    for( int32_t i = nTipFilterBlocks - 1; i >= 0  && pIndexSearch; i--, pIndexSearch = pIndexSearch->pprev ) {
        aFilterPoint.nBlockTime = pIndexSearch->GetBlockTime();
        // aFilterPoint.nDiffBits = fDiffPrevFromHash ? pIndexSearch->GetBlockHash() : pIndexSearch->nBits;
        aFilterPoint.nDiffBits = pIndexSearch->nBits;
        aFilterPoint.nSpacing = aFilterPoint.nSpacingError = aFilterPoint.nRateOfChange = 0;
        vIndexTipFilter.push_back( aFilterPoint );
    }

    //! Sort the TipFilter block data by time.  The result is then setup as an output vector of structures
    //! containing all the filter information which can be accessed and referenced as needed.
    assert( vIndexTipFilter.size() == nTipFilterBlocks );            //! The array of strutures is constant in size and assumed.
    sort(vIndexTipFilter.begin(), vIndexTipFilter.end());                 //! Thank you sort routine, now it matters not the time order in which the blocks were mined
    uint32_t nDividerSum = 0;
    arith_uint256 uintBlockPOW;
    uintPrevDiffCalculated.SetHex("0x0");
    //! Process the difficulty values
    for( int32_t i = 1; i <= nTipFilterBlocks; i++ ) {
        uintBlockPOW.SetCompact( vIndexTipFilter[i - 1].nDiffBits );
        uintBlockPOW *= (uint32_t)i;
        uintPrevDiffCalculated += uintBlockPOW;
        nDividerSum += i;                           //! Bump the weighted sum, the newer it is the more it counts
    }
    nPrevDiffWeight = nDividerSum;
    uintPrevDiffCalculated /= nDividerSum;
    
    nDividerSum = 0;
    nWeightedAvgTipBlocksUp = 4;
    if( pIndex->nHeight > HARDFORK_BLOCK2 )
        nWeightedAvgTipBlocksUp = WEIGHTEDAVGTIPBLOCKS_UP;

    assert(nWeightedAvgTipBlocksUp <= nTipFilterBlocks);
    uintTipDiffCalculatedUp.SetHex("0x0");
    for( int32_t i = nTipFilterBlocks - nWeightedAvgTipBlocksUp + 1; i <= nTipFilterBlocks; i++ ) { //CSlave: Calculate a weighted moving average on the partial tip for diff UP
        uintBlockPOW.SetCompact( vIndexTipFilter[i - 1].nDiffBits );
        uintBlockPOW *= (uint32_t)(i + nWeightedAvgTipBlocksUp - nTipFilterBlocks);
        uintTipDiffCalculatedUp += uintBlockPOW;
        nDividerSum += i + nWeightedAvgTipBlocksUp - nTipFilterBlocks;   //! Bump the weighted sum, the newer it is the more it counts
    }
    uintTipDiffCalculatedUp /= nDividerSum;

    nDividerSum = 0;
    nWeightedAvgTipBlocksDown = 6;
    if( pIndex->nHeight > HARDFORK_BLOCK2 )
        nWeightedAvgTipBlocksDown = WEIGHTEDAVGTIPBLOCKS_DOWN;

    assert(nWeightedAvgTipBlocksDown <= nTipFilterBlocks);
    uintTipDiffCalculatedDown.SetHex("0x0");
    for( int32_t i = nTipFilterBlocks - nWeightedAvgTipBlocksDown + 1; i <= nTipFilterBlocks; i++ ) { //CSlave: Calculate a weighted moving average on the partial tip for diff DOWN
        uintBlockPOW.SetCompact( vIndexTipFilter[i - 1].nDiffBits );
        uintBlockPOW *= (uint32_t)(i + nWeightedAvgTipBlocksDown - nTipFilterBlocks);
        uintTipDiffCalculatedDown += uintBlockPOW;
        nDividerSum += i + nWeightedAvgTipBlocksDown - nTipFilterBlocks;   //! Bump the weighted sum, the newer it is the more it counts
    }
    uintTipDiffCalculatedDown /= nDividerSum;


    //! Once we know the tipfilter has been setup, an output calculation is likely to soon follow,
    //! plus we now have 2 ways to define the previous difficulty.  Whichever method is chosen,
    //! defines the maximum increase and maximum decrease limit values.
    // Use the previous block in the index.
    // if( GetBoolArg( "-retargetpid.limitfromprevblock", false ) )
    // CSlave: hardcoded to use the smoothed difficulty value of both partial tipfilter UP and DOWN
    // ...do not forget the diff is inverse, retarget UP is a smaller diff target...
        uintPrevDiffForLimitsLast.SetCompact( pIndex->nBits );    //Previous difficulty of the last block, useful to know if Diff goes UP or DOWN
        uintPrevDiffForLimitsTipUp = uintTipDiffCalculatedUp;     //Previous difficulty calculated on the partial tip blocks selected for diff UP
        uintPrevDiffForLimitsTipDown = uintTipDiffCalculatedDown; //Previous difficulty calculated on the partial tip blocks selected for diff DOWN

    if( pIndex->nHeight > HARDFORK_BLOCK2 ) { 
        nMaxDiffIncrease = NMAXDIFFINCREASE2;
        nMaxDiffDecrease = NMAXDIFFDECREASE2;
    }

    if (nMaxDiffIncrease <= 101 ) {
        LogPrintf("Error: nMaxDiffIncrease <= 101, DiffAtMaxIncrease is set to * 1.01 \n");
        nMaxDiffIncrease = 101;
    }
    uintPrevDiffForLimitsIncreaseLast = uintPrevDiffForLimitsLast * 100; //For a quick increase of difficulty, let's take the previous block diff
    uintDiffAtMaxIncreaseLast = uintPrevDiffForLimitsIncreaseLast / nMaxDiffIncrease;
    uintPrevDiffForLimitsIncreaseTip = uintPrevDiffForLimitsTipUp * 100; //For a smoothed increase of difficulty, let's take the diff from the partial tip blocks
    uintDiffAtMaxIncreaseTip = uintPrevDiffForLimitsIncreaseTip / nMaxDiffIncrease;

    // CSlave: Here is enhanced the accuracy for the maxdiffincrease and maxdiffdecrease limits. Instead of using units we now use hundredths (percents).
    // The minimum value for the difficulty retarget limits is thus set to 101% which is equivalent to a 1.01 multiplier or divider.

    if (nMaxDiffDecrease <= 101 ) {
        LogPrintf("Error: nMaxDiffDecrease <= 101, DiffAtMaxDecrease is set to / 1.01 \n");
        nMaxDiffDecrease = 101;
    }
    
    uintPrevDiffForLimitsDecreaseLast = uintPrevDiffForLimitsLast * nMaxDiffDecrease; //For a quick decrease of difficulty, let's take the diff from the previous block diff
    uintDiffAtMaxDecreaseLast = uintPrevDiffForLimitsDecreaseLast / 100;
    uintPrevDiffForLimitsDecreaseTip = uintPrevDiffForLimitsTipDown * nMaxDiffDecrease; //For a smoothed decrease of difficulty, let's take the diff from the partial tip blocks
    uintDiffAtMaxDecreaseTip = uintPrevDiffForLimitsDecreaseTip / 100;

    //! If fUsesHeader is set, we update the spacing errors and rate of change results each time a new header time is given.
    //! If not, then the spacing error and rate of change results can be done now, and will be ready when this call returns.
    if( !fUsesHeader )
        UpdateFilterTimingResults( vIndexTipFilter );

    //! Remember what Height we last made these calculations
    nIndexFilterHeight = pIndex->nHeight;
    //! Signal this retargetpid is ready to process output calculations
    fTipFilterInitialized = true;

    return fTipFilterInitialized;
}

//! This is one of the hardest steps to do well for crypto coins in general.  It is also a difficult
//! step to do well for process controllers when discrete samples are filled with noise.  Solutions
//! which improve controller response attempt to model the nature of that noise, yet any action taken
//! will introduce lag into the error calculation.  That can not be helped, as we must consider possible
//! noise sources which include human intent.  What this does is work with the most recent 11 blocks or
//! 10 block time spans at the tip.  For Anoncoin @ 3min target spacing that = 30 minutes. That gives us
//! enough information to calculate both a block timing error and the differential error in one step.
bool CRetargetPidController::SetBlockTimeError( const CBlockIndex* pIndex, const CBlockHeader* pBlockHeader )
{
    bool fResult = false;                                           //! Until we know the error has been calculated
    int64_t nNewBlockTime = pBlockHeader->GetBlockTime();           //! We are about to calculate the errors for this new header time
    if( UpdateIndexTipFilter( pIndex ) ) {
        //! The last entry into the TipTimesFilter will be set to nTipTime in the CalcBlockTimeErrors() method and
        //! the final error calculations done, we also store those results in our retargetpid objects data fields for later processing
        //! by the OutputUpdate routine.  No error will be ever be returned by the call to CalcBlockTimeErrors() from here.
        fResult = CalcBlockTimeErrors( nNewBlockTime );
        nLastCalculationTime = nNewBlockTime;
    }
    return fResult;
}

/********************************************//**
 * \brief The primary integral calculation
 *
 * \param pIndex const CBlockIndex* Only the blockindex is required
 * \return bool true of the calculation is ready for use
 *
 ***********************************************/
bool CRetargetPidController::ChargeIntegrator( const CBlockIndex* pIndex )       //! Returns true if the full integration time was able to done
{
    if( !pIndex || !pIndex->pprev )                 //! Its over if we do not have enough data to even start
        return false;

    //! The integrator does not care what the next block time is, instantaneous error is not its concern.
    //! If this is being called because that has changed, yet our previous calculation was done for this
    //! same block height, we are done and already have the integrator value calculated.
    if( nIntegratorHeight != pIndex->nHeight ) {    //! The pIndex value is changed throughout this routine, save the height now.
        nIntegratorHeight = pIndex->nHeight;        //! Remember when we last calculated this
        pChargedToIndex = pIndex;                   //! Remember the pointer to, so we don't need to search for it when restoring a previous state.
    }
    else                                            //! We have the results for this height already
        return true;

    //! The nIntegrationTime can be set to zero
    if( !nIntegrationTime ) {
        nBlocksSampled = 0;
        nIntegratorChargeTime = 0;
        dIntegratorBlockTime = (double)nTargetSpacing;
        return true;
    }

    //! And some interm values used in the loop
    const int64_t nMostRecentBlockTime = pIndex->GetBlockTime();
    const int64_t nOldestBlockTime = nMostRecentBlockTime - nIntegrationTime;
    int64_t nBlockTime;

    //! Initialize the number of samples we have taken.  For any case greater than 2 blocks, a failure to
    //! have a sufficient BlockIndex will cause this next loop to exit with the samples taken set correctly.
    nBlocksSampled = 1;

    //! This loop always gets executed at least once, or we would not have made it this far.
    //! If we run into a previous blockindex that is null or what normally happens is the
    //! block time about to be added to the sample set, has a time that is less that the
    //! nIntegrationTime defined by the user and as started with the MostRecentBlockTime.
    //! When that is found, it falls through the loop with what data it was able to gather.
    do {
        pIndex = pIndex->pprev;                                 //! Always moving backwards to the previous block from the starting blockindex entry given
        nBlockTime = pIndex->GetBlockTime();                    //! Get a local copy of the absolute time when this block was mined
        nBlocksSampled++;                                       //! Maintain an accurate count of the blocks we have sampled
    } while( pIndex->pprev && nOldestBlockTime < pIndex->pprev->GetBlockTime() );
    //! If we're about to hit a block with a time older than our integration period, do not include it,
    //! it could be the genesis block and ancient, which leads to a period of 678+days of blocktime summed

    //! Calc how much time has past between this data point and the starting blockindex entry given
    nIntegratorChargeTime = nMostRecentBlockTime - nBlockTime;

    //! At this point the amount of time that has past, the sum of all the difficulties
    //! as well as the number of blocks we sampled are all known values.
    //! Given that information, the Integrator term (should be) now easy to calculate and ready
    //! for use as part of the new pid retarget output.  Save the results for the next steps...
    dIntegratorBlockTime = (double)nIntegratorChargeTime / (double)(nBlocksSampled - 1);

    if (dIntegratorBlockTime < DMININTEGRATOR && nIntegratorHeight <= HARDFORK_BLOCK2) { 
        dIntegratorBlockTime = DMININTEGRATOR;    //! Capped to prevent integrator windup   
    } else if (dIntegratorBlockTime < DMININTEGRATOR2 && nIntegratorHeight > HARDFORK_BLOCK2) {
        dIntegratorBlockTime = DMININTEGRATOR2;
    } else if (dIntegratorBlockTime > DMAXINTEGRATOR && nIntegratorHeight <= HARDFORK_BLOCK2) {
        dIntegratorBlockTime = DMAXINTEGRATOR;    
    } else if (dIntegratorBlockTime > DMAXINTEGRATOR2 && nIntegratorHeight > HARDFORK_BLOCK2) {
        dIntegratorBlockTime = DMAXINTEGRATOR2;
    }
    return true;
}

/**
 *  Difficulty formula Calculation - Throughout its life and well into 2015, due in part to the known flaws &
 *  weakness exploits to exist in the KGW algo.  A new PID controller Re-Targeting Engine was born.  Invented
 *  and introduced here for the 1st time, by its developer... GroundRod
 *
 *  THE MAIN RetargetPID functionality comes from calling this class method.
 */
bool CRetargetPidController::UpdateOutput( const CBlockIndex* pIndex, const CBlockHeader* pBlockHeader, const Consensus::Params& params )
{
    const arith_uint256 &uintPOWlimit = UintToArith256(params.powLimit);

    if( IsPidUpdateRequired(pIndex, pBlockHeader) ) {
        if( !ChargeIntegrator(pIndex) || !SetBlockTimeError(pIndex, pBlockHeader) ) {
            //! For cases where we can not calculate the PID output, we set the result to the minimum difficulty.
            uintTargetAfterLimits = uintPOWlimit;
            return false;
        }

    if( pIndex->nHeight > HARDFORK_BLOCK2 ) {
        dProportionalGain=PID_PROPORTIONALGAIN2;
        nIntegrationTime=PID_INTEGRATORTIME2;
        dIntegratorGain=PID_INTEGRATORGAIN2;
        dDerivativeGain=PID_DERIVATIVEGAIN2;
    }

        //! We can now calculate the controllers output time, but not the dimensionless number which is divided by nTargetSpacing and
        //! applied to the new overall Proof-of-Work that will be required as the minimum.
        //! That final calculation here in float math yields a value somewhere around '1' and would not convert to unsigned integer
        //! math in any meaningful way.  Short term errors will show up here as a correction to the time output, thanks to the 'P'
        //! and 'D' terms times whatever you have their gains set to.
        dProportionalTerm = dProportionalGain * dSpacingError;

        //! This next line of code is gold.  GR
        //! Although there is no proof-of-work behind it, a great deal of 'my' time was spent before realizing it was
        //! just this simple.  Not at all easy to come up with, and then finally decide to use.  That is why here all
        //! you see is it being assignment to a new local variable.  It is worth being written as 10K lines of code...
        //! CSlave: GR simple equation was latter changed to include a dIntegratorGain to settle at the setpoint
        dIntegratorTerm = (dIntegratorBlockTime - (double)nTargetSpacing) * dIntegratorGain + (double)nTargetSpacing;

        //! The derivative term
        dDerivativeTerm = dDerivativeGain * dRateOfChange;

        //! Finally!  The pid controller loop output value
        dPidOutputTime = dProportionalTerm + dIntegratorTerm + dDerivativeTerm;

        //! Now we convert double back into an unsigned integer, so it can be used with the uint256 class math.  Only one multiply and divide
        //! need be done.
        nPidOutputTime = roundint64( dPidOutputTime );  //! Round up or down as required, see util.h for that one, our resolution here now will be to the nearest second.
        uintTargetBeforeLimits = uintPrevDiffCalculated;
        //! The Pid Output time should definitely be positive now, as the integrator value will dominate.  However if the gain
        //! were set to high while the instantaneous error is very large, it could be possible for it to be negative.  We do not want the uint256 math
        //! to be multiplying by any value smaller than 1 second or it is out of range by design.  The POWlimit sets the design limit on the
        //! other end, for which no difficulty can be less then.  That is checked for all the algorithm's in the parent routines final output stage.
        if( nPidOutputTime < 1 ) {
            nPidOutputTime = 1;     // From here only the positive whole number of seconds is used in the calculation of output difficulty adjustment
            dPidOutputTime = 1.0;   // Also make sure the PidOutputTime (double) value does not show a negative either, although it is only used for reporting after this
            fPidOutputLimited = true;
        } else
            fPidOutputLimited = false;

        //! The final new minimum Proof-Of-Work value can now be calculated, and if necessary, range limited to Anoncoin's minimum.
        uintTargetBeforeLimits *= (uint32_t)nPidOutputTime;
        uintTargetBeforeLimits /= (uint32_t)nTargetSpacing;

        //! Now we can place limits on the amount of change allowed, based only on the most recent past block, and the bounds set by the software
        fDifficultyLimited = LimitOutputDifficultyChange( uintTargetAfterLimits, uintTargetBeforeLimits, uintPOWlimit, pIndex);
    }
    return true;
}

void CRetargetPidController::RunReports( const CBlockIndex* pIndex, const CBlockHeader *pBlockHeader, const Consensus::Params& params )
{
    const arith_uint256 &uintPOWlimit = UintToArith256(params.powLimit);

    if( !UpdateOutput( pIndex, pBlockHeader, params ) )
        return;

    //! We report the current spacing, and keep track of the sum and count for a debugging column which runs from the moment this retargetpid was
    //! created until the program is terminated or a new external reset command is given.
    const int64_t nCurrentSpacing = nLastCalculationTime - pIndex->GetBlockTime();

    //! Only log the pid statistics when they have been recomputed & changed.  Plus only log the Integrator precharge data the 1st time it is computed
    //! Setup for writing to diagnostic spreadsheet
    ofstream csvfile;
    if( gArgs.GetBoolArg("-retargetpid.retargetcsv", false) ) {
        boost::filesystem::path pathRetarget = GetDataDir() / "retarget.csv";
        csvfile.open( pathRetarget.string().c_str(), ofstream::out | ofstream::app );
    }

    if( fRetargetNewLog ) {
        LogPrintf( "RetargetPID-v3.0 NextWorkRequired for TargetSpacing=%d using constants PropGain=%f, IntTime=%d, IntGain=%f and DevGain=%f\n",
                  nTargetSpacing, dProportionalGain, nIntegrationTime, dIntegratorGain, dDerivativeGain );
        LogPrintf( "Integrator Charged for=%d days %02d:%02d:%02d with %d samples",
                   nIntegratorChargeTime / SECONDSPERDAY, (nIntegratorChargeTime % SECONDSPERDAY) / 3600,
                   (nIntegratorChargeTime % 3600) / 60, nIntegratorChargeTime % 60, nBlocksSampled );
        LogPrintf( ". Actual BlockTime=%fsecs\n", dIntegratorBlockTime );
        if( csvfile.is_open() ) {
            csvfile << "OS_Time" << "," << "Offset" << "," << "Height" << ",";
            csvfile << "MinTime" << "," << "IndexTime" << "," << "BlockTime" << "," << "Space" << ",";
            csvfile << "TipsAvg" << "," << "<--" << ",";
            csvfile << "SpaceErr" << ",";
            csvfile << "ICharge" << "," << "IBlocks" << ",";
            csvfile << "RateOfChg" << ",";
            csvfile << "-->" << "," << "PropTerm" << "," << "IntTerm" << "," << "DerTerm" << "," << "PIDout" << ",";
            if( fLogDiffLimits )
                csvfile << "LimPrev" << "," << "LimUpDiff" << "," << "LimDnDiff" << ",";
            csvfile << "PrevDiff" << "," << "NewDiff" << ",";
            csvfile << "NetKHPS" << "," << "MineKHPS" << ",";
            csvfile << "PrevLog2" << "," << "NewLog2" << "," << "ChainLog2" << ",";
#if defined( HARDFORK_BLOCK )
            if( Params().isMainNetwork() && pIndex->nHeight < HARDFORK_BLOCK ) {
#else
            if( Params().isMainNetwork() ) {
#endif
                csvfile << "KgwDiff" << "," << "KgwLog2" << ",";
            }
            csvfile << "PID_Difficulty_as_256_bits" << "\n";
        }
        fRetargetNewLog = false;
    }

    //! Always produce a minimum of debug.log information
    LogPrintf("RetargetPID charged to height=%d output terms P=%f I=%f D=%f, ProofOfWork Required=0x%08x Header=0x%08x\n",
              nIntegratorHeight, dProportionalTerm, dIntegratorTerm, dDerivativeTerm, uintTargetAfterLimits.GetCompact(), pBlockHeader->nBits );

    if( fPidOutputLimited )
        LogPrintf("RetargetPID NOTE: OutputTime %f was < 1 second, out-of-range value set to %d.\n", dPidOutputTime, nPidOutputTime );

    if( fDifficultyLimited )
        LogPrintf("RetargetPID NOTE: Difficulty %0x was out of range and set to limit %0x\n", uintTargetBeforeLimits.GetCompact(), uintTargetAfterLimits.GetCompact());

// #if defined( HARDFORK_BLOCK )
    //if( isMainNetwork() && pIndex->nHeight < HARDFORK_BLOCK )
// #else
    //if( isMainNetwork() )
// #endif
        //LogPrint("retarget", "Prev KGW: %08x %s\n", pIndex->nBits, arith_uint256().SetCompact(pIndex->nBits).ToString());
    //else
        //LogPrint("retarget", "  Before: %08x %s\n", pIndex->nBits, arith_uint256().SetCompact(pIndex->nBits).ToString());
    //LogPrint("retarget", "  After : %08x %s\n", uintTargetBeforeLimits.GetCompact(), uintTargetBeforeLimits.ToString());

    if( csvfile.is_open() && ( /*nIntegratorHeight > Checkpoints::GetTotalBlocksEstimate() ||*/ gArgs.GetBoolArg("-retargetpid.logallblocks", false) ) ) {
        csvfile << GetTime() << "," << GetTimeOffset() << "," << nIntegratorHeight << ",";
        csvfile << (pIndex->GetMedianTimePast() + 1) << "," << pIndex->GetBlockTime() << "," << nLastCalculationTime << "," << nCurrentSpacing << ",";
        csvfile << dAverageTipSpacing << ",,";
        csvfile << dSpacingError << ",";
        csvfile << nIntegratorChargeTime << "," << nBlocksSampled << ",";
        csvfile << dRateOfChange << ",";
        string sErrors;
        if( fPidOutputLimited ) sErrors += "+";
        if( fDifficultyLimited ) sErrors += "*";
        if( sErrors.size() == 0 ) sErrors = "ok";
        csvfile << sErrors << "," << dProportionalTerm << "," << dIntegratorTerm << "," << dDerivativeTerm << ",";
        csvfile <<  dPidOutputTime << ",";
        //! If enabled, Calculate and add the limit difficulties
        if( fLogDiffLimits )
            csvfile << GetLinearWork(uintPrevDiffForLimitsLast, uintPOWlimit) << "," << GetLinearWork(uintDiffAtMaxIncreaseTip, uintPOWlimit) << "," << GetLinearWork(uintDiffAtMaxDecreaseTip, uintPOWlimit) << ",";
        //! Calculate and add the linear Difficulty calculations
        csvfile << GetLinearWork(uintPrevDiffCalculated, uintPOWlimit) << "," << GetLinearWork(uintTargetAfterLimits, uintPOWlimit) << ",";
        //! Include Details about Network Hashes per second and Miners HashMeter results if they are running
        double dNetKHPS = CalcNetworkHashPS(pIndex, nTipFilterBlocks) / 1000.0;
        double dMinerKHPS = GetFastMiningKHPS();
        csvfile << dNetKHPS << "," << dMinerKHPS << ",";
        //! Calculate and add the Log2 work calculations
        csvfile << GetLog2Work(uintPrevDiffCalculated) << "," << GetLog2Work(uintTargetAfterLimits) << "," << GetLog2Work(pIndex->nChainWork) << ",";
#if defined( HARDFORK_BLOCK )
        if( Params().isMainNetwork() && pIndex->nHeight <= HARDFORK_BLOCK ) {
#else
        if( Params().isMainNetwork() ) {
#endif
            //! Add the KGW value for work, it is simply the nBits as found in the current blockheader
            arith_uint256 KgwDiff;
            KgwDiff.SetCompact(pBlockHeader->nBits);
            csvfile << GetLinearWork(KgwDiff, uintPOWlimit) <<  "," << GetLog2Work(KgwDiff) <<  ",";
        }
        csvfile << "\"0x" << uintTargetAfterLimits.ToString() << "\"" << "\n";
        csvfile.close();

        //! Generate a predictive difficulty curve for the next new block
        if( gArgs.GetBoolArg("-retargetpid.diffcurves", false) ) {
            boost::filesystem::path pathDiffCurves = GetDataDir() / "diffcurves.csv";
            csvfile.open( pathDiffCurves.string().c_str(), ofstream::out | ofstream::app );
            //! The diffcurves.csv output will have at least these columns:
            //! Height, IndexTime, TipTime, Spacing, Pterm, Iterm, Dterm , PiOut, PidOut, PiLog2, PidLog2, PiDiff, PidDiff
            if( fDiffCurvesNewLog ) {
                csvfile << "Height" << "," << "IndexTime" << "," << "F" << "," << "TipTime" << "," << "Space" << ",";
                csvfile << "PTerm" << "," << "ITerm" << ","<< "DTerm" << ","<< "PIout" << ","<< "PIDout" << ",";
                csvfile << "PIlog2" << "," << "PIDlog2" << ",";
                csvfile << "PIdiff" <<  "," << "PIDdiff" << "\n";
                fDiffCurvesNewLog = false;
            }

            bool fBestTime = false;
            bool fActualTime = false;
            bool fFullLine = false;
            int64_t nSecsPerSample = nTargetSpacing / 6;
            int64_t nTimeOfCalc;

            assert(nSecsPerSample > 0 );
            const int64_t nMinTime = pIndex->GetMedianTimePast()+1;
            const int64_t nLastBlockIndexTime = pIndex->GetBlockTime();
            const int64_t nNextBestBlockTime = nLastBlockIndexTime + nTargetSpacing;
            const int64_t nMaxTime = nLastBlockIndexTime + nTargetSpacing * 8;  // 7 intervals past the best time
            double dProportionalCalc, dDerivativeCalc;
            arith_uint256 uintDiffPi, uintDiffPid;
            arith_uint256 uintDiffCalc;
            // string sFlags;

            for( int64_t nTipTime = nMinTime; nTipTime <= nMaxTime; nTipTime += nSecsPerSample ) {
#if defined( DONT_COMPILE )
            for( int64_t nTipTime = nLastBlockIndexTime - 3 * nSecsPerSample; nTipTime <= nMaxTime; nTipTime += nSecsPerSample ) {
                if( !fUsesHeader ) {    // No point in running curves, we're done
                    if( !fBestTime )
                        fBestTime = true;
                    else
                        break;
                }
#endif
                if( !fBestTime ) {
                    nTimeOfCalc = nNextBestBlockTime;
                    fBestTime = fFullLine = true;
                } else if( !fActualTime ) {
                    nTimeOfCalc = nLastCalculationTime;
                    fActualTime = fFullLine = true;
                } else
                    nTimeOfCalc = nTipTime;

                CalcBlockTimeErrors( nTimeOfCalc );
                dProportionalCalc = dProportionalGain * dSpacingError;
                dDerivativeCalc = dDerivativeGain * dRateOfChange;

                //! Run the PI controller calculations
                int64_t nOutputTimePi = roundint64( dProportionalCalc + dIntegratorTerm );
                if( nOutputTimePi < 1 ) nOutputTimePi = 1;
                uintDiffCalc = uintPrevDiffCalculated;
                uintDiffCalc *= (uint32_t)nOutputTimePi;
                uintDiffCalc /= (uint32_t)nTargetSpacing;
                LimitOutputDifficultyChange( uintDiffPi, uintDiffCalc, uintPOWlimit, pIndex);
                double dNewLog2Pi = GetLog2Work( uintDiffPi );
                double dNewDiffPi = GetLinearWork(uintDiffPi, uintPOWlimit);

                //! Run the PID controller calculations
                int64_t nOutputTimePid = roundint64( dProportionalCalc + dIntegratorTerm + dDerivativeCalc );
                if( nOutputTimePid < 1 ) nOutputTimePid = 1;
                uintDiffCalc = uintPrevDiffCalculated;
                uintDiffCalc *= (uint32_t)nOutputTimePid;
                uintDiffCalc /= (uint32_t)nTargetSpacing;
                LimitOutputDifficultyChange( uintDiffPid, uintDiffCalc, uintPOWlimit, pIndex);
                double dNewLog2Pid = GetLog2Work( uintDiffPid );
                double dNewDiffPid = GetLinearWork(uintDiffPid, uintPOWlimit);

                //! Log the result calculations
                if( fFullLine ) {
                    csvfile << nIntegratorHeight << "," << nLastBlockIndexTime << "," << (fActualTime ? "@" : "#") << ",";
                    nTipTime -= nSecsPerSample;
                    fFullLine = false;
                } else
                    csvfile << nIntegratorHeight << "," << "," << ",";
                csvfile << nTimeOfCalc << "," << (nTimeOfCalc - nLastBlockIndexTime) << ",";
                csvfile << dProportionalCalc << "," << dIntegratorTerm << "," << dDerivativeCalc << "," << nOutputTimePi << "," << nOutputTimePid << ",";
                csvfile << dNewLog2Pi << "," << dNewLog2Pid << ",";
                csvfile << dNewDiffPi << "," << dNewDiffPid << "\n";
            }
            csvfile.close();
        }
    }
}

/********************************************//**
 * \brief Returns the number of blocks required to integrate over 1 full Integration Time period.
 *
 * \param pIndex const CBlockIndex*  If this value is NULL an estimate is returned
 * \return uint32_t
 *         The number of blockindex entries that will be required to cover the IntegrationTime span or the ideal as an estimate
 *
 ***********************************************/
uint32_t CRetargetPidController::CalcBlockIndexRequired( const CBlockIndex* pIndex )
{
    LOCK( cs_retargetpid );

    if( !pIndex || !pIndex->pprev )                             //! Its over if we do not have enough data to even start
        return (uint32_t)(nIntegrationTime / nTargetSpacing);   //! Return a close approximation to the number of blocks that would be required

    const int64_t nOldestBlockTime = pIndex->GetBlockTime() - nIntegrationTime;

    //! Initialize the number of samples we have taken.  For any case greater than 2 blocks, a failure to
    //! have a sufficient BlockIndex will cause this next loop to exit with the samples taken set correctly.
    uint32_t nBlkSam = 1;
    //! Loop back in time through the blocks looking for the last one which is less than the oldest required.
    do {
        pIndex = pIndex->pprev;                                 //! Always moving backwards to the previous block from the active chain tip
        nBlkSam++;                                              //! Maintain an accurate count of the blocks we are sampling
    } while( pIndex->pprev && nOldestBlockTime < pIndex->pprev->GetBlockTime() );

    return nBlkSam;
}

//! Return a copy of all the current state parameters
int32_t CRetargetPidController::GetTipFilterSize( void )
{
    LOCK( cs_retargetpid );

    int32_t nResult = nTipFilterBlocks;
    if( fUsesHeader ) nResult++;
    return nResult;
}

//! Return a copy of all the current state parameters
bool CRetargetPidController::GetRetargetStats( RetargetStats& RetargetState, uint32_t& nHeight, const CBlockIndex* pIndexAtTip, const Consensus::Params& params )
{
    LOCK( cs_retargetpid );

    //! At least store all the static constant, infrequently changing and non-output variables details in the result structure
    RetargetState.dProportionalGain = dProportionalGain;
    RetargetState.nIntegrationTime = nIntegrationTime;
    RetargetState.dIntegratorGain = dIntegratorGain;
    RetargetState.dDerivativeGain = dDerivativeGain;
    RetargetState.fUsesHeader = fUsesHeader;
    RetargetState.nTipFilterSize = nTipFilterBlocks;
    if( fUsesHeader ) RetargetState.nTipFilterSize++;
    RetargetState.nMaxDiffIncrease = nMaxDiffIncrease;
    RetargetState.nMaxDiffDecrease = nMaxDiffDecrease;
    RetargetState.nPrevDiffWeight = nPrevDiffWeight;
    RetargetState.nSpacingErrorWeight = nSpacingErrorWeight;
    RetargetState.nRateChangeWeight = nRateChangeWeight;

    const CBlockIndex* pPrevCharge = pChargedToIndex;
    uint32_t nPrevChargeHeight = nIntegratorHeight;     //! Remember where the pid Integrator and filter calculations was set to before this command.

    //! Make sure we can even run the calculations, otherwise the above state values is all that we can copy and provide as valid
    if( pIndexAtTip == NULL || pIndexAtTip->nHeight < nTipFilterBlocks || (nHeight != 0 && nHeight <= nTipFilterBlocks ) )
        return false;

    //! If height=0 is passed, use the height at the tip, also if the height given - 1 is equal to the current charge height, we do not need to setup a different one
    //! If the height being reported is greater than the height of the index tip, create a dummy header with the timestamp for this moment,
    //! otherwise, pull the validated header information from the block index data itself, and run the calculations with that block time
    const CBlockIndex* pIndexCharge = NULL;
    CBlockHeader aHeader;
    if( nHeight == 0 || nHeight > pIndexAtTip->nHeight ) {
        nHeight = pIndexAtTip->nHeight + 1;
        aHeader.nTime = GetAdjustedTime();
        pIndexCharge = pIndexAtTip;
    } else {
        const CBlockIndex* pIndexAtHeight = pIndexAtTip;
        while( pIndexAtHeight->nHeight > nHeight )
            pIndexAtHeight = pIndexAtHeight->pprev;
        aHeader = pIndexAtHeight->GetBlockHeader();
        //! In the case where the Header is used, the nBits needs to be zero, so the getretargetpid query can identify the block, if fUsesHeader is false
        //! then the only thing used for reporting is the block time anyway.
        aHeader.nBits = 0;
        pIndexCharge = pIndexAtHeight->pprev;
    }

    if( UpdateOutput( pIndexCharge, &aHeader, params ) ) {
        RetargetState.nMinTimeAllowed = pIndexCharge->GetMedianTimePast() + 1;
        RetargetState.nLastCalculationTime = nLastCalculationTime;
        RetargetState.nIntegratorHeight = nIntegratorHeight;
        RetargetState.nBlocksSampled = nBlocksSampled;
        RetargetState.nIntegratorChargeTime = nIntegratorChargeTime;
        RetargetState.dSpacingError = dSpacingError;
        RetargetState.dRateOfChange = dRateOfChange;
        RetargetState.dProportionalTerm = dProportionalTerm;
        RetargetState.dIntegratorTerm = dIntegratorTerm;
        RetargetState.dDerivativeTerm = dDerivativeTerm;
        RetargetState.dPidOutputTime = dPidOutputTime;
        RetargetState.uintPrevDiff = uintPrevDiffCalculated;
        RetargetState.fPidOutputLimited = fPidOutputLimited;
        RetargetState.fDifficultyLimited = fDifficultyLimited;
        RetargetState.uintTargetDiff = uintTargetAfterLimits;
        RetargetState.dAverageTipSpacing = dAverageTipSpacing;
        RetargetState.vTipFilter = fUsesHeader ? vTipFilterWithHeader : vIndexTipFilter;
        RetargetState.nBlockSpacing = nLastCalculationTime - pIndexCharge->GetBlockTime();
        RetargetState.uintPrevShad = UintToArith256(pIndexCharge->GetBlockPoWHash());
    }
    // Restore the Integrator charge and filter calculations to previous settings, before we unlock the retarget pid
    if( nPrevChargeHeight != 0 && pPrevCharge && pPrevCharge != pChargedToIndex ) {
        ChargeIntegrator(pPrevCharge);
        UpdateIndexTipFilter(pPrevCharge);
    }
    return true;
}


//! Only the following global functions are seen from the outside world and used throughout
//! the rest of the source code for Anoncoin, everything above should be static or defined
//! within the CRetargetPID class.

//! The workhorse routine, which oversees BlockChain Proof-Of-Work difficulty retarget algorithms
unsigned int GetNextWorkRequired2(const CBlockIndex* pindexLast, const CBlockHeader* pBlockHeader, const Consensus::Params& params)
{
    //! Any call to this routine needs to have at least 1 block and the header of a new block.
    //! ...NULL pointers are not allowed...GR
    assert(pindexLast);
    assert( pBlockHeader );

    //! All networks now always calculate the RetargetPID output, it needs to have been setup
    //! during initialization, if unit tests are being run, perhaps where the genesis block
    //! has been added to the index, but no pRetargetPID setup yet then we detect it here and
    //! return minimum difficulty for the next block, in any other case the programmer should
    //! have already at least created a RetargetPID class object and set the master pointer up.
    arith_uint256 uintResult;
    if( pRetargetPid ) {
        //! Under normal conditions, update the PID output and return the next new difficulty required.
        //! We do this while locked, once the Output Result is captured, it is immediately unlocked.
        //! Based on height, perhaps during a blockchain initial load, other older algos will need to
        //! be run, and their result returned.  That is detected below and processed accordingly.
        {
            LOCK( cs_retargetpid );
            if( !pRetargetPid->UpdateOutput( pindexLast, pBlockHeader, params) )
                LogPrintf("Insufficient BlockIndex, unable to set RetargetPID output values.\n");
            uintResult = pRetargetPid->GetRetargetOutput(); //! Always returns a limit checked valid result
        }
        //! Testnets always use the P-I-D Retarget Controller, only the MAIN network might not...
        if( Params().isMainNetwork() ) {
            if( pindexLast->nHeight > nDifficultySwitchHeight3 ) {      //! Start of KGW era
                //! The new P-I-D retarget algo will start at this hardfork block + 1
                if( pindexLast->nHeight <= nDifficultySwitchHeight4 )   //! End of KGW era
                    uintResult = NextWorkRequiredKgwV2(pindexLast, params);     //! Use fast v2 KGW calculator
            } else
                uintResult = OriginalGetNextWorkRequired(pindexLast);   //! Algos Prior to the KGW era
        }
    } else
        uintResult = UintToArith256(params.powLimit);

    //! Finish by converting the resulting uint256 value into a compact 32 bit 'nBits' value
    return uintResult.GetCompact();
}

void RetargetPidReset( string strParams, const CBlockIndex* pIndex, const Consensus::Params& params )
{
    LOCK( cs_retargetpid );

    bool fCreateNew = true;
    double dPropGain, dPropGainNow;
    int64_t nIntTime, nIntTimeNow;
    double dIntGain, dIntGainNow;
    double dDevGain, dDevGainNow;
    istringstream issParams(strParams);
    try {
        issParams >> dPropGain >> nIntTime >> dIntGain >> dDevGain;
        if( pRetargetPid ) {
            pRetargetPid->GetPidTerms( &dPropGainNow, &nIntTimeNow, &dIntGainNow, &dDevGainNow );
            //! Check to see if we have already reset the PID controller to the new values, if so do not keep executing a reset
            if( (dPropGainNow == dPropGain) && (nIntTimeNow == nIntTime) && (dIntGainNow == dIntGain) && (dDevGainNow == dDevGain) )
                fCreateNew = false;
            else
                delete pRetargetPid;
        }
    } catch( const std::exception& ) {
        fCreateNew = false;
    }

    if( fCreateNew ) {
        pRetargetPid = new CRetargetPidController( dPropGain, nIntTime, dIntGain, dDevGain, params );
        pRetargetPid->ChargeIntegrator(pIndex);
        pRetargetPid->UpdateIndexTipFilter(pIndex);
        //! At this point mining can resume and reporting will begin as if it was a new start.
    } else
        LogPrintf( "While Resetting RetargetPID Parameters, the values matched current settings or an error was thrown while reading them.\n" );
}

//! This routine handles lock and diagnostics as well as charging the Integrator after
//! a new block has been processed and verified, or any other time the Tip() changes.
//!
//! We also run reports and update the TipFilter values, so future calls to calculate a new output retarget result can be done fast.
bool SetRetargetToBlock( const CBlockIndex* pIndex, const Consensus::Params& params )
{
    //! A null pointer shows up here when running test_anoncoin while loading the genesis block, do nothing.
    if( !pRetargetPid )
        return false;

    LOCK( cs_retargetpid );

    //! When this is called we have just updated the tip block or have finished loading the blockchain index during initialization.
    //! We want to now report all the results from the previous block height, as if this current new index entry was a new header.
    //! This means being one block behind in reporting, but in every other regard works well for when fUsesHeader is true or false.
    if( pIndex && pIndex->pprev ) {
        CBlockHeader aHeader = pIndex->GetBlockHeader();
        pRetargetPid->RunReports( pIndex->pprev, &aHeader, params );
    } else
        return false;

    //! Making it this far means we can now finally charge the Integror and setup the TipFilter to height as requested by the caller
    bool fResult1 = pRetargetPid->ChargeIntegrator(pIndex);
    bool fResult2 = pRetargetPid->UpdateIndexTipFilter(pIndex);

    //! Now we build a string with the correct text for use in Log reporting output,
    //! that will primarily be based on the build condition and network selected.
    //! Although if UsesHeader() is true the next blocks difficuly is always changing
    //! and so a computation is made by calling GetNextWorkRequired() using a header
    //! time of the present moment, which works of coarse for any network and regardless
    //! of wither or not a header time is important for the retarget output value.  This
    //! then is used to log the health and result of the retargetpid, for a next new
    //! block.
    bool fBasedOnKGW = false;
    string sNextWorkRequired;

    if( Params().isMainNetwork() ) {
        //! The new algo will happen at the hardfork block + 1, otherwise its an old KGW block.
        int32_t nDistance = nDifficultySwitchHeight4 - pIndex->nHeight;
        fBasedOnKGW = nDistance >= 0;
        if( nDistance > 0 ) {         // likekly KGW is being used
            sNextWorkRequired = ( pIndex->nHeight > nDifficultySwitchHeight3 ) ?
                                  strprintf( "For this and next %s blocks, ProofOfWork based on KGW. Required=", nDistance ) :
                                  "Next ProofOfWork based on old algo. Required=";
        }
        else if( nDistance == 0 )   // Last KGW block
            sNextWorkRequired = "Last Block based on KGW. ProofOfWork Required=";
        else {                      // RetargetPID is used
            sNextWorkRequired = strprintf( "For %d blocks ProofOfWork based on RetargetPID, Next Required=", -nDistance );
            //! A special case exists for when the 1st next new block is about to be mined, This triggers an event
            //! within the HARDFORK build, so all non-whitelisted nodes get disconnected and private forks of
            //! the main chain can be run for evaluation purposes.
            // if( nDistance == 1 )
        }
    } else
        sNextWorkRequired = "Next ProofOfWork Required=";

    if( !fBasedOnKGW && fResult1 && fResult2 && pRetargetPid->UsesHeader() ) sNextWorkRequired += "dynamic RightNow=";

    //! Create a dummy header, with the present moment as the block time. calculate the Next Work Required and report that.
    CBlockHeader aHeader;
    aHeader.nTime = GetAdjustedTime();
    uint32_t nNextBits = GetNextWorkRequired(pIndex, &aHeader, params);
    sNextWorkRequired += strprintf( "0x%08x", nNextBits );

    LogPrintf("RetargetPID %s to height=%d, tipfilter %s, %s\n", fResult1 ? "charged" : "Integrator failed charge", pIndex->nHeight, fResult2 ? "updated" : "update failed", sNextWorkRequired );

    return fResult1 && fResult2;
}

