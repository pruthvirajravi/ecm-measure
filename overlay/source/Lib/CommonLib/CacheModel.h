/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.
 *
 * Copyright (c) 2010-2023, ITU/ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ITU/ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file     CacheModel.h
    \brief    general cache class (header)
*/


#ifndef _CACHEMODEL_H_
#define _CACHEMODEL_H_
#include "Picture.h"

// function list
#if !JVET_J0090_MEMORY_BANDWITH_MEASURE
#define JVET_J0090_SET_CACHE_ENABLE( enable )                 /* do nothing */
#define JVET_J0090_SET_REF_PICTURE( refPic, compID )          /* do nothing */
#define JVET_J0090_CACHE_ACCESS( src, fileName, line )        /* do nothing */
#define JVET_AM0295_PUSH_CACHE_ENABLE( enable )               /* do nothing */
#define JVET_AM0295_PUSH_CACHE_ENABLE2( enable )              /* do nothing */
#define JVET_AM0295_POP_CACHE_ENABLE()                        /* do nothing */
#define JVET_AM0295_POP_CACHE_ENABLE_TM()                     /* do nothing */
#define JVET_AM0295_COND_PUSH_CACHE_ENABLE( cond, enable )    /* do nothing */
#define JVET_AM0295_COND_PUSH_CACHE_ENABLE_TM( cond, enable ) /* do nothing */
#define JVET_AM0295_COND_POP_CACHE_ENABLE( cond )             /* do nothing */
#define JVET_AM0295_COND_POP_CACHE_ENABLE_TM( cond )          /* do nothing */
#else
#define JVET_J0090_SET_CACHE_ENABLE( enable )                 if (m_cacheModel) m_cacheModel->setCacheEnable( enable )
#define JVET_J0090_SET_REF_PICTURE( refPic, compID )          if (m_cacheModel) m_cacheModel->setRefPicture( refPic, compID )
#define JVET_J0090_CACHE_ACCESS( src, fileName, line )        if (m_cacheModel) m_cacheModel->cacheAccess( src, fileName, line )
#define JVET_AM0295_PUSH_CACHE_ENABLE( enable )               if (m_cacheModel) m_cacheModel->setTmpCacheEnable( enable )
#define JVET_AM0295_PUSH_CACHE_ENABLE2( enable )              if (m_interRes.m_cacheModel) m_interRes.m_cacheModel->setTmpCacheEnable( enable )
#define JVET_AM0295_POP_CACHE_ENABLE()                        if (m_cacheModel) m_cacheModel->restoreCacheEnable()
#define JVET_AM0295_POP_CACHE_ENABLE_TM()                     if (m_interRes.m_cacheModel) m_interRes.m_cacheModel->restoreCacheEnable()
#define JVET_AM0295_COND_PUSH_CACHE_ENABLE( cond, enable )    if (cond) JVET_AM0295_PUSH_CACHE_ENABLE( enable )
#define JVET_AM0295_COND_PUSH_CACHE_ENABLE_TM( cond, enable ) if (cond) JVET_AM0295_PUSH_CACHE_ENABLE2( enable )
#define JVET_AM0295_COND_POP_CACHE_ENABLE( cond )             if (cond) JVET_AM0295_POP_CACHE_ENABLE()
#define JVET_AM0295_COND_POP_CACHE_ENABLE_TM( cond )          if (cond) JVET_AM0295_POP_CACHE_ENABLE_TM()
#include <stack>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>
// ADR-026 K4 step 0 (D092): per-call-site audit of which reference accesses the J0090 model counts vs skips.
// Additive instrumentation only; no effect on decoding. g_j0090Ctx: 0 other, 1 DecCu::xDeriveCUMV, 2 DecCu::xReconInter, 3 DecCu::xReconIntraQT.
extern int g_j0090Ctx;
// ADR-027 Phase 0 (D095): position-level dump. g_j0090Pu = PU whose reference accesses are being made (set by DecCu around
// xDeriveCUMV / xReconInter); its live mv/refIdx identify the candidate under evaluation at the time of each access.
struct PredictionUnit; extern const PredictionUnit* g_j0090Pu;
struct J0090CtxGuard { int prev; J0090CtxGuard( int c ) { prev = g_j0090Ctx; g_j0090Ctx = c; } ~J0090CtxGuard() { g_j0090Ctx = prev; } };
#if JVET_J0090_MEMORY_BANDWITH_MEASURE
#define J0090_CTX( c ) J0090CtxGuard _j0090ctxGuard( c )
#else
#define J0090_CTX( c )
#endif
class CacheModel
{
private:
  // cache enable
  std::map<std::string,int64_t> m_siteCounted, m_siteSkipped, m_siteMiss, m_siteOOR;   // OOR: addr outside the current reference buffer (D095 audit)
  // ADR-026 K4 step 1 (council #22 controls): separate caches fed only by reconstruction (B) or only by derivation (A)
  // accesses -> incremental demand; and per-frame unique-line sets -> compulsory-miss floor.
  CacheModel*   m_subRecon  = nullptr;
  CacheModel*   m_subDerive = nullptr;
  bool          m_isSub     = false;
  std::unordered_set<uint64_t> m_uniqA, m_uniqB, m_uniqAll;
  // ADR-026 K4 step 2: LRU stack-distance (reuse-distance) histogram per class, in distinct cache lines, within a frame.
  struct ReuseDist
  {
    std::unordered_map<uint64_t, int> last;   // key -> time index (1-based) of last access
    std::vector<int> fen;                     // Fenwick tree over time indices: 1 if that time is the latest access of its key
    int t = 0; uint64_t lastKey = ~0ull;
    int64_t hist[40] = {0}; int64_t first = 0; int64_t total = 0;   // hist[b]: accesses with distance in (2^(b-1), 2^b] lines
    void reset( int cap ) { last.clear(); fen.assign( cap + 2, 0 ); t = 0; lastKey = ~0ull; }
    void add( int i ) { for ( ; i < (int) fen.size(); i += i & -i ) fen[i]++; }
    void sub( int i ) { for ( ; i < (int) fen.size(); i += i & -i ) fen[i]--; }
    int  sum( int i ) { int s = 0; for ( ; i > 0; i -= i & -i ) s += fen[i]; return s; }
    void access( uint64_t key );
  };
  ReuseDist m_rdA, m_rdB, m_rdAll;
  int64_t m_rdHistA[40] = {0}, m_rdHistB[40] = {0}, m_rdHistAll[40] = {0}; int64_t m_rdFirstA = 0, m_rdFirstB = 0, m_rdFirstAll = 0;
  int64_t       m_uniqASeq = 0, m_uniqBSeq = 0, m_uniqAllSeq = 0;
  std::string   m_cfgName;
  bool          xAccessCore( size_t cacheAddr );
  // ADR-027 Phase 0 dump: one record per (picture, CU, ctx, live mv/refIdx, ref POC, component) = header + sorted distinct
  // 16-sample (32-byte) raster segment ids of the reference picture; any line size / 2-D mapping is derived offline.
  struct DumpHdr { int32_t curPoc, refPoc; uint16_t cuX, cuY, cuW, cuH; uint8_t ctx, comp, interDir, pad0; int32_t mv[4]; int8_t ref[2]; uint16_t pad1; uint32_t stride, nseg;
                   bool same( const DumpHdr& o ) const { return curPoc==o.curPoc && refPoc==o.refPoc && cuX==o.cuX && cuY==o.cuY && cuW==o.cuW && cuH==o.cuH && ctx==o.ctx && comp==o.comp && interDir==o.interDir && mv[0]==o.mv[0] && mv[1]==o.mv[1] && mv[2]==o.mv[2] && mv[3]==o.mv[3] && ref[0]==o.ref[0] && ref[1]==o.ref[1] && stride==o.stride; } };
  bool          m_oorSkip = false;
  // D095 address-resolution fix (J0090_FIX=1): every access is resolved against the registry of known picture buffers.
  // (a) inside the current reference buffer -> as before; (b) inside another reference picture of the current slice ->
  // legitimate reference access, re-attributed to that picture (ECM calls the filter for templates without updating the
  // model's reference picture); (c) inside the current picture -> not a reference-memory access (J0090 convention), skipped;
  // (d) elsewhere (stack / intermediate heap buffers, e.g. second-stage interpolation input) -> skipped.
  struct BufReg { const Pel* base; size_t extent; int poc; int comp; int stride; bool isCur; };
  bool          m_fix = false;
  std::vector<BufReg> m_bufs; const Picture* m_curPic = nullptr;
  int64_t       m_nReattr = 0, m_nCurPic = 0, m_nTmp = 0, m_nResolved = 0; std::map<std::string,int64_t> m_siteReattr, m_siteCur, m_siteTmp;
  void          xRegisterPicture( const Picture* pic, bool isCur );
  const BufReg* xResolve( const Pel* addr );
  void          rawAccessResolved( size_t cacheAddr, int poc, ComponentID comp );
  FILE*         m_dump = nullptr; DumpHdr m_dumpHdr{}; bool m_dumpHave = false; std::unordered_set<uint32_t> m_dumpSegs; int64_t m_dumpRecords = 0;
  void          xDumpAccess( const Pel* addr, const Pel* base, int poc, ComponentID comp, int stride );
  void          xDumpFlush();
public:
  void          rawAccess( const Pel *addr );
  int64_t       missSeq() const { return m_missHitCountSeq; }
private:
  bool          m_cacheEnable;
  bool          m_cacheEnableFilterUsing;
  std::stack<bool>          m_cacheEnableFilterTmp;
  bool          m_cacheEnableFilter;
  // report level
  bool          m_frameReport;
  // cache parameters
  int           m_cacheLineSize;   // size of byte in each entry (shall be power of 2)
  int           m_numCacheLine;    // # of cache line
  int           m_numWay;          // # of way
  int           m_cacheSize;       // total entry numer (line number * way)
  int           m_cacheAddrMode;   // cache address mode
  int           m_cacheBlkWidth;   // block width in 2D access
  int           m_cacheBlkHeight;  // block height in 2D access
  // cache parameters for address calc
  int           m_shift;
  // cache entry
  size_t*       m_cacheAddr;
  int*          m_cachePoc;
  ComponentID*  m_cacheComp;
  bool*         m_available;
  // access Information
  int           m_refPoc;
  Pel*          m_base;
  ComponentID   m_compID;
  int           m_picWidth;
  // PLRU parameters
  int           m_treeDepth;
  int*          m_treeStatus;

  // stastical infromation for a frame
  int64_t*          m_hitCount; // for each cache entry
  int64_t           m_missHitCount; // for calc total bandwidth
  int64_t           m_totalAccess;
  // stastical infromation for a sequence
  int64_t       m_hitCountSeq;
  int64_t       m_missHitCountSeq;
  int64_t       m_totalAccessSeq;
  int           m_frameCount;

public:
  CacheModel();
  ~CacheModel();
  bool isCacheEnable( ) { return m_cacheEnable; }
  void create(const std::string& cacheCfgFileName);
  void destroy( );
  void clear( );
  void reportFrame();
  void reportSequence();
  void cacheAccess( const Pel *addr, const std::string& fileName, const int lineNum );
  void accumulateFrame( );
  void setCacheEnable( bool enable );
  void setTmpCacheEnable( bool enable );
  void restoreCacheEnable();
  void setRefPicture( const Picture *refPic, const ComponentID compID );

protected:
  bool xIsCacheHit( int pos, size_t addr );
  int xCalcTreeSize( int way );
  int xCalcPower( int num );
  int xGetWay( int entry );
  size_t xMapAddress( size_t offset );
  void xConfigure(const std::string& filename);
  void xUpdateCache( int entry, size_t addr );
  void xUpdateCacheStatus( int entry, int way );
  // PLRU
  int xGetWayTreePLRU( int entry );
  void xUpdatePLRUStatus( int entry, int way );
};

#endif // JVET_J0090_MEMORY_BANDWITH_MEASURE
#endif // _CACHEMODEL_H_
