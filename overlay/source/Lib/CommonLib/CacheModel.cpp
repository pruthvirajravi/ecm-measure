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

/** \file     CacheModel.cpp
    \brief    general cache class
*/

#ifdef _MSC_VER
#pragma warning(disable:4996)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <inttypes.h>
#define __STDC_FORMAT_MACROS
#ifdef WIN32
#define strdup _strdup
#endif

#include "Utilities/program_options_lite.h"
#include "CacheModel.h"
#include <algorithm>
#include "Unit.h"
#include "Slice.h"
#if JVET_J0090_MEMORY_BANDWITH_MEASURE

#ifndef JVET_J0090_MEMORY_BANDWITH_MEASURE_PRINT_ACCESS_INFO
#define JVET_J0090_MEMORY_BANDWITH_MEASURE_PRINT_ACCESS_INFO 0
#endif
#ifndef JVET_J0090_MEMORY_BANDWITH_MEASURE_PRINT_FRAME
#define JVET_J0090_MEMORY_BANDWITH_MEASURE_PRINT_FRAME -1
#endif

int g_j0090Ctx = 0;
const PredictionUnit* g_j0090Pu = nullptr;

// ADR-027 Phase 0: position-level dump (env J0090_DUMP=<file>; main cache only; counted accesses only).
void CacheModel::xDumpFlush()
{
  if ( !m_dumpHave || !m_dump ) return;
  std::vector<uint32_t> segs( m_dumpSegs.begin(), m_dumpSegs.end() );
  std::sort( segs.begin(), segs.end() );
  m_dumpHdr.nseg = (uint32_t) segs.size();
  fwrite( &m_dumpHdr, sizeof( DumpHdr ), 1, m_dump );
  if ( !segs.empty() ) fwrite( segs.data(), sizeof( uint32_t ), segs.size(), m_dump );
  m_dumpRecords++;
  m_dumpSegs.clear(); m_dumpHave = false;
}

void CacheModel::xDumpAccess( const Pel* addr, const Pel* base, int poc, ComponentID comp, int stride )
{
  DumpHdr h{};
  h.refPoc = poc; h.ctx = (uint8_t) g_j0090Ctx; h.comp = (uint8_t) comp; h.stride = (uint32_t) stride;
  if ( g_j0090Pu )
  {
    const PredictionUnit& pu = *g_j0090Pu;
    h.curPoc = pu.cu->slice->getPOC();
    h.cuX = (uint16_t) pu.lx(); h.cuY = (uint16_t) pu.ly(); h.cuW = (uint16_t) pu.lwidth(); h.cuH = (uint16_t) pu.lheight();
    h.interDir = (uint8_t) pu.interDir;
    h.mv[0] = pu.mv[0].hor; h.mv[1] = pu.mv[0].ver; h.mv[2] = pu.mv[1].hor; h.mv[3] = pu.mv[1].ver;
    h.ref[0] = (int8_t) pu.refIdx[0]; h.ref[1] = (int8_t) pu.refIdx[1];
  }
  else { h.curPoc = -1; h.ref[0] = h.ref[1] = -1; }
  if ( m_dumpHave && !m_dumpHdr.same( h ) ) xDumpFlush();
  if ( !m_dumpHave ) { m_dumpHdr = h; m_dumpHave = true; }
  m_dumpSegs.insert( (uint32_t) ( (size_t) ( addr - base ) >> 4 ) );
}

void CacheModel::ReuseDist::access( uint64_t key )
{
  if ( key == lastKey ) return;            // consecutive tap reads of the same line carry no reuse information
  lastKey = key; total++;
  if ( t + 2 >= (int) fen.size() ) return; // capacity guard (counted in total, not in hist)
  t++;
  auto it = last.find( key );
  if ( it == last.end() ) { first++; }
  else
  {
    int d = sum( t - 1 ) - sum( it->second );   // distinct lines touched since the previous access of this key
    int b = 0; while ( ( 1LL << b ) < d && b < 39 ) b++;
    hist[b]++;
    sub( it->second );
  }
  add( t ); last[key] = t;
}


enum CacheAddressMap
{
  CACHE_MODE_1D = 0,
  CACHE_MODE_2D,
  MAX_NUM_CACHE_MODE
};

namespace po = df::program_options_lite;

void* cache_mem_align_malloc(int size, int alignSize)
{
  unsigned char *alignBuf;
  unsigned char *buf = (unsigned char *)malloc(size + 2 * alignSize + sizeof(void **));
  if (buf)
  {
    alignBuf = buf + alignSize + sizeof(void **);
    alignBuf -= (intptr_t)alignBuf & (alignSize - 1);
    *((void **)(alignBuf - sizeof(void **))) = buf;
    return alignBuf;
  }
  return nullptr; // memory keep fail
}

void cache_mem_align_free(void *ptr)
{
  if ( ptr )
  {
    free(*(((void **)ptr) - 1));
  }
}

CacheModel::CacheModel()
{
  m_cacheEnable       = false;
  m_cacheEnableFilter = false;
  m_cacheLineSize     = 0;
  m_numCacheLine      = 0;
  m_numWay            = 0;
  m_cacheSize         = 0;
  m_shift             = 0;
  m_cacheAddr         = nullptr;
  m_cachePoc          = nullptr;
  m_cacheComp         = nullptr;
  m_available         = nullptr;
  m_refPoc            = 0;
  m_base              = nullptr;
  m_compID            = MAX_NUM_COMPONENT;
  m_hitCount          = nullptr;
  m_treeStatus        = nullptr;
  m_missHitCount      = 0;
  m_totalAccess       = 0;
  m_hitCountSeq       = 0;
  m_missHitCountSeq   = 0;
  m_totalAccessSeq    = 0;
  m_frameCount        = 0;
}

CacheModel::~CacheModel()
{
}

int CacheModel::xCalcPower( int num )
{
  int power = -1;

  for ( int i = 0 ; i < 32 ; i++ )
  {
    if ( num == (1 << i) )
    {
      power = i;
      break;
    }
  }
  if (power < 0)
  {
    THROW("non power of 2");
  }
  return power;
}

void CacheModel::xConfigure(const std::string& filename )
{
  po::Options opts;
  opts.addOptions()
  ("CacheEnable",   m_cacheEnable,   false, "Cache Enable" )
  ("CacheLineSize", m_cacheLineSize,   128, "Cache line size")
  ("NumCacheLine",  m_numCacheLine,     32, "Number of cache line")
  ("NumWay",        m_numWay,            4, "Number of way")
  ("CacheAddrMode", m_cacheAddrMode,     0, "Address mapping mode 0 : linear address 1 : 2D address")
  ("BlkWidth",      m_cacheBlkWidth,    32, "Block width in 2D address mode")
  ("BlkHeight",     m_cacheBlkHeight,   16, "Block height in 2D address mode")
  ("FrameReport",   m_frameReport,   false, "Report in each frame" )
  ;

  po::setDefaults(opts);
  po::parseConfigFile( opts, filename );

  if ( m_cacheLineSize > CACHE_MEM_ALIGN_SIZE )
  {
    fprintf( stderr, "cache line size is bigger that memory alignment\n" );
    fprintf( stderr, "This may lead mismatch among enviroments\n" );
  }
  if ( m_cacheAddrMode == CACHE_MODE_2D )
  {
    int blkSize = m_cacheBlkWidth * m_cacheBlkHeight;
    if ( m_cacheLineSize % blkSize != 0 && blkSize % m_cacheLineSize ) {
      THROW("CacheLineSize shall be multiple of BlkWidth x BlkHeight or BlkWidth x BlkHeight shall be multiple of CacheLineSize in 2D mode");
    }
  }
}

// initilize cache information such as size
void CacheModel::create(const std::string& cacheCfgFileName)
{
  bool init = cacheCfgFileName == "";

  if (cacheCfgFileName.length() > 1000)
  {
    THROW("config file name for cache model is too long. It shall be < 1000\n");
  }
  if ( init )
  {
    return; // no cache config
  }
  xConfigure(cacheCfgFileName);
  m_cfgName = cacheCfgFileName;

  if ( !m_cacheEnable )
  {
    return;
  }
  if ( !m_isSub )
  {
    m_subRecon = new CacheModel(); m_subRecon->m_isSub = true; m_subRecon->create( cacheCfgFileName );
    m_subDerive = new CacheModel(); m_subDerive->m_isSub = true; m_subDerive->create( cacheCfgFileName );
    m_fix = getenv( "J0090_FIX" ) != nullptr; if ( m_fix ) printf( "J0090: D095 address resolution ON (re-attribute to other reference pictures; skip current-picture and intermediate-buffer reads)\n" );
    m_oorSkip = getenv( "J0090_OOR_SKIP" ) != nullptr; if ( m_oorSkip ) printf( "J0090: out-of-range accesses are NOT counted (J0090_OOR_SKIP)\n" );
    if ( const char* dumpPath = getenv( "J0090_DUMP" ) )
    {
      m_dump = fopen( dumpPath, "wb" );
      if ( m_dump ) { const char magic[8] = { 'J','0','9','0','D','P','0','1' }; fwrite( magic, 1, 8, m_dump ); }
      printf( "J0090 dump: %s (%s)\n", dumpPath, m_dump ? "open" : "FAILED" );
    }
  }

  // set parameters
  m_cacheSize = m_numCacheLine * m_numWay;
  // calc address calculation parameter
  m_shift = xCalcPower( m_cacheLineSize );
  // keep memory
  m_cacheAddr  = new size_t [m_cacheSize];
  m_cachePoc   = new int [m_cacheSize];
  m_cacheComp  = new ComponentID [m_cacheSize];
  m_available  = new bool  [m_cacheSize];
  m_hitCount   = new int64_t  [m_cacheSize];
  // PLRU
  m_treeDepth  = xCalcPower( m_numWay );
  m_treeStatus = new int [m_numCacheLine];
  if ( m_cacheLineSize > 0 && m_numCacheLine > 0 && m_numWay > 0 )
  {
    m_cacheEnableFilter = true;
  }
}

// free memory
void CacheModel::destroy()
{
  if ( m_dump ) { xDumpFlush(); fclose( m_dump ); m_dump = nullptr; printf( "J0090 dump: %" PRId64 " records written\n", m_dumpRecords ); }
  if ( m_cacheAddr )
  {
    delete [] m_cacheAddr;
  }
  if ( m_cachePoc )
  {
    delete [] m_cachePoc;
  }
  if ( m_cacheComp )
  {
    delete [] m_cacheComp;
  }
  if ( m_available )
  {
    delete [] m_available;
  }
  if ( m_hitCount )
  {
    delete [] m_hitCount;
  }
  if ( m_treeStatus )
  {
    delete [] m_treeStatus;
  }
}

// clear cache status (set invalid for each entry)
void CacheModel::clear()
{
  if ( m_cacheEnable )
  {
    ::memset( m_available, 0, m_cacheSize * sizeof(bool) );
    ::memset( m_hitCount,  0, m_cacheSize * sizeof(int64_t) );
    m_missHitCount = 0;
    m_totalAccess  = 0;
    if ( m_subRecon )  m_subRecon->clear();
    if ( m_subDerive ) m_subDerive->clear();
    m_uniqA.clear(); m_uniqB.clear(); m_uniqAll.clear();
    if ( !m_isSub ) { m_rdA.reset( 1 << 25 ); m_rdB.reset( 1 << 25 ); m_rdAll.reset( 1 << 26 ); }
  }
}

// accuulate result for sequence level
void CacheModel::accumulateFrame( )
{
  if ( m_cacheEnable )
  {
    for ( int i = 0 ; i < m_cacheSize ; i++ )
    {
      m_hitCountSeq += m_hitCount[ i ];
    }
    m_missHitCountSeq += m_missHitCount;
    m_totalAccessSeq  += m_totalAccess;
    if ( m_subRecon )  m_subRecon->accumulateFrame();
    if ( m_subDerive ) m_subDerive->accumulateFrame();
    m_uniqASeq += (int64_t) m_uniqA.size(); m_uniqBSeq += (int64_t) m_uniqB.size(); m_uniqAllSeq += (int64_t) m_uniqAll.size();
    for ( int b = 0; b < 40; b++ ) { m_rdHistA[b] += m_rdA.hist[b]; m_rdHistB[b] += m_rdB.hist[b]; m_rdHistAll[b] += m_rdAll.hist[b]; m_rdA.hist[b] = m_rdB.hist[b] = m_rdAll.hist[b] = 0; }
    m_rdFirstA += m_rdA.first; m_rdFirstB += m_rdB.first; m_rdFirstAll += m_rdAll.first; m_rdA.first = m_rdB.first = m_rdAll.first = 0;

    if ( m_totalAccessSeq < 0 )
    {
      fprintf( stdout, "detect overflow\n" );
    }
  }
}

// report bandwidth, hit ratio and so on in a Frame
void CacheModel::reportFrame( )
{
  if ( m_cacheEnable )
  {
    if ( m_frameReport )
    {
      int64_t hitCount = 0;

      for ( int i = 0 ; i < m_cacheSize ; i++ )
      {
        hitCount += m_hitCount[ i ];
      }

      fprintf( stdout, "Cache Statics in frame %d\n", m_frameCount );
      fprintf( stdout, "Hit ratio %5.2f [%%]\n", 100 * (((double)hitCount / m_totalAccess)) );
      fprintf( stdout, "Required bandwidth %.1f [MB]\n", ((double)(m_missHitCount) / (1024 * 1024) * m_cacheLineSize) );
    }
    m_frameCount++;
  }
}

void CacheModel::reportSequence( )
{
  if ( m_cacheEnable )
  {
    fprintf( stdout, "Cache config\n" );
    fprintf( stdout, "Cache line size: %d\n", m_cacheLineSize  );
    fprintf( stdout, "Cache line number %d\n", m_numCacheLine );
    fprintf( stdout, "Cache way number %d\n\n", m_numWay );

    fprintf( stdout, "Cache Statics in total\n" );
    fprintf( stdout, "Hit ratio %5.2f [%%]\n", (100 * (double)(m_hitCountSeq)) / m_totalAccessSeq );
#ifdef _MSC_VER
    fprintf( stdout, "Hit count / total %I64d / %I64d\n", m_hitCountSeq, m_totalAccessSeq );
#else
    fprintf( stdout, "Hit count / total %" PRIi64 " / %" PRIi64 "\n", m_hitCountSeq, m_totalAccessSeq );
#endif
    fprintf( stdout, "Required bandwidth %.1f [MB] / frame\n", (((double)m_missHitCountSeq) * m_cacheLineSize) / (m_frameCount * 1024 * 1024) );
    fprintf( stdout, "CONTROL recon-only-cache misses %" PRIi64 "  derive-only-cache misses %" PRIi64 "  shared misses %" PRIi64 "  frames %d  line %d\n",
             m_subRecon ? m_subRecon->missSeq() : (int64_t) -1, m_subDerive ? m_subDerive->missSeq() : (int64_t) -1, m_missHitCountSeq, m_frameCount, m_cacheLineSize );
    fprintf( stdout, "FLOOR unique-lines-per-sequence A %" PRIi64 "  B %" PRIi64 "  all %" PRIi64 "\n", m_uniqASeq, m_uniqBSeq, m_uniqAllSeq );
    fprintf( stdout, "REUSE-DISTANCE histograms (deduped accesses; bin b = distance in (2^(b-1),2^b] distinct lines; FIRST = first touch in frame)\n" );
    fprintf( stdout, "RD A FIRST %" PRIi64 " :", m_rdFirstA ); for ( int b = 0; b < 32; b++ ) fprintf( stdout, " %" PRIi64, m_rdHistA[b] ); fprintf( stdout, "\n" );
    fprintf( stdout, "RD B FIRST %" PRIi64 " :", m_rdFirstB ); for ( int b = 0; b < 32; b++ ) fprintf( stdout, " %" PRIi64, m_rdHistB[b] ); fprintf( stdout, "\n" );
    fprintf( stdout, "RD ALL FIRST %" PRIi64 " :", m_rdFirstAll ); for ( int b = 0; b < 32; b++ ) fprintf( stdout, " %" PRIi64, m_rdHistAll[b] ); fprintf( stdout, "\n" );
    fprintf( stdout, "\nJ0090 call-site audit (ctx|file:line  counted  skipped  misses) ctx: 0 other 1 xDeriveCUMV 2 xReconInter 3 xReconIntraQT\n" );
    std::map<std::string,int64_t> keys( m_siteCounted ); for ( auto &kv : m_siteSkipped ) keys[ kv.first ] += 0;
    for ( auto &kv : keys )
    {
      fprintf( stdout, "SITE %s %" PRIi64 " %" PRIi64 " %" PRIi64 "\n", kv.first.c_str(), m_siteCounted[ kv.first ], m_siteSkipped[ kv.first ], m_siteMiss[ kv.first ] );
    }
    for ( auto &kv : m_siteOOR ) fprintf( stdout, "OOR %s %" PRIi64 "\n", kv.first.c_str(), kv.second );
    if ( m_fix )
    {
      fprintf( stdout, "RESOLVE counted-in-reference %" PRIi64 "  reattributed-to-other-ref %" PRIi64 "  current-picture-skipped %" PRIi64 "  intermediate-buffer-skipped %" PRIi64 "\n", m_nResolved, m_nReattr, m_nCurPic, m_nTmp );
      for ( auto &kv : m_siteReattr ) fprintf( stdout, "REATTR %s %" PRIi64 "\n", kv.first.c_str(), kv.second );
      for ( auto &kv : m_siteCur )    fprintf( stdout, "CURPIC %s %" PRIi64 "\n", kv.first.c_str(), kv.second );
      for ( auto &kv : m_siteTmp )    fprintf( stdout, "TMPBUF %s %" PRIi64 "\n", kv.first.c_str(), kv.second );
    }
  }
}

void CacheModel::setRefPicture( const Picture *refPic, const ComponentID CompID )
{
    m_refPoc = refPic->getPOC();
    m_base   = refPic->getOrigin( PIC_RECONSTRUCTION, CompID );
    m_compID = CompID;
    m_picWidth = refPic->getRecoBuf( CompID ).stride;
    if ( m_subRecon )  m_subRecon->setRefPicture( refPic, CompID );
    if ( m_subDerive ) m_subDerive->setRefPicture( refPic, CompID );
}

bool CacheModel::xIsCacheHit( int pos, size_t addr )
{
  bool ret = false;

  if ( m_available[pos] )
  {
    if ( addr == m_cacheAddr[pos] )
    {
      if ( m_refPoc == m_cachePoc[pos] )
      {
        if ( m_compID == m_cacheComp[pos] )
        {
          ret = true;
        }
      }
    }
  }
  return ret;
}

//-- PLRU

int CacheModel::xGetWayTreePLRU( int entry )
{
  int shift  = 0;
  int way    = 0;
  for ( int i = 0; i < m_treeDepth ; i++ )
  {
    int flag  = (m_treeStatus[ entry ] >> shift) & 0x1;
    shift = (shift << 1) + flag + 1;
    way   = (way << 1) | (flag ^ 0x1);
  }
  xUpdateCacheStatus( entry, way );

  return way;
}

void CacheModel::xUpdatePLRUStatus( int entry, int way )
{
  int val   = m_treeStatus[ entry ];
  int shift = 0;

  for ( int i = 0 ; i < m_treeDepth ; i++ )
  {
    int flag = (way >> (m_treeDepth - i - 1)) & 0x1;
    val = (val & (~0 ^ (1 << shift))) | (flag << shift); // only set shift-th bit
    shift = (shift << 1) + 2 - flag;
  }

  m_treeStatus[ entry ] = val;
}

//-- other cache alg. (for future use)

// get update way based on each update algorithm (Now Tree PLRU only)
int CacheModel::xGetWay( int entry )
{
  // single way
  if ( m_numWay == 1 )
  {
    return 0;
  }
  // multiway
  return xGetWayTreePLRU( entry );
}

// update cache entry
void CacheModel::xUpdateCache( int entry, size_t addr )
{
  int way = xGetWay( entry );

  if (entry * m_numWay + way >= m_cacheSize || entry * m_numWay + way < 0)
  {
    THROW("incorrect cache info");
  }

  m_cacheAddr[ entry * m_numWay + way ] = addr;
  m_cachePoc[ entry * m_numWay + way ]  = m_refPoc;
  m_cacheComp[ entry * m_numWay + way ] = m_compID;
  m_available[ entry * m_numWay + way ] = true;

}

void CacheModel::xUpdateCacheStatus( int entry, int way )
{
  if ( m_numWay == 1 )
  {
    return;
  }
  xUpdatePLRUStatus( entry, way );
}

size_t CacheModel::xMapAddress( size_t offset ) {

  size_t ret;
  size_t xInPic, yInPic, blkPosX, blkPosY, xInBlk, yInBlk;

  switch ( m_cacheAddrMode ) {
    case CACHE_MODE_1D : // diret mapping
      return offset;
    case CACHE_MODE_2D : // 2D address mapping
      xInPic  = offset % m_picWidth;
      yInPic  = offset / m_picWidth;
      blkPosX = xInPic / m_cacheBlkWidth;
      blkPosY = yInPic / m_cacheBlkHeight;
      xInBlk  = xInPic % m_cacheBlkWidth;
      yInBlk  = yInPic % m_cacheBlkHeight;
      ret  = m_picWidth * blkPosY * m_cacheBlkHeight;
      ret += blkPosX * m_cacheBlkWidth * m_cacheBlkHeight;
      ret += yInBlk * m_cacheBlkWidth;
      ret += xInBlk;
      return ret;
    default :
      THROW( "Unknown address mode " << m_cacheAddrMode );
      return 0;
  }
}

// check cache hit/miss
void CacheModel::cacheAccess( const Pel *addr, const std::string& fileName, const int lineNum )
{
  if ( !m_cacheEnable )
  {
    return;
  }
  const std::string site = std::to_string( g_j0090Ctx ) + "|" + fileName.substr( fileName.find_last_of( "/\\" ) + 1 ) + ":" + std::to_string( lineNum );
  if ( !m_cacheEnableFilter )
  {
    m_siteSkipped[ site ]++;
    return;
  }
  m_siteCounted[ site ]++;
  { const ptrdiff_t off = addr - m_base;
    if ( off < -(ptrdiff_t) m_picWidth * 200 || off > (ptrdiff_t) m_picWidth * 4800 )
    {
      m_siteOOR[ site ]++;
      { const long long a = off < 0 ? -off : off; const int cls = a > 1000000000LL ? 3 : a > 10000000LL ? 2 : a > 1000000LL ? 1 : 0; m_siteOOR[ "CLASS" + std::to_string( cls ) + "|ctx" + std::to_string( g_j0090Ctx ) ]++; }
      if ( m_oorSkip ) return;                                   // crude variant: drop every out-of-range access
    }
  }
  // D095 resolution
  const Pel* base = m_base; int poc = m_refPoc; ComponentID comp = m_compID; int stride = m_picWidth;
  if ( m_fix )
  {
    if ( m_bufs.empty() && g_j0090Pu ) xResolve( addr );         // populate registry
    bool inCur = ( addr >= m_base && m_base && addr < m_base + ( (size_t) m_picWidth * 4800 ) ); // fast path check refined below
    const BufReg* r = xResolve( addr );
    if ( r == nullptr )            { m_nTmp++;    m_siteTmp[ site ]++; return; }
    if ( r->isCur )                { m_nCurPic++; m_siteCur[ site ]++; return; }
    if ( r->base != m_base )       { m_nReattr++; m_siteReattr[ site ]++; base = r->base; poc = r->poc; comp = (ComponentID) r->comp; stride = r->stride; }
    (void) inCur; m_nResolved++;
  }
  if ( m_dump ) xDumpAccess( addr, base, poc, comp, stride );
  size_t cacheAddr = xMapAddress( (size_t) (addr - base) ) >> m_shift;
  const bool isA = ( g_j0090Ctx == 1 || g_j0090Ctx == 11 || g_j0090Ctx == 12 || g_j0090Ctx == 13 );
  const bool isB = ( g_j0090Ctx == 2 || g_j0090Ctx == 21 || g_j0090Ctx == 22 || g_j0090Ctx == 23 );
  const uint64_t key = ( (uint64_t) (poc + 4096) << 44 ) ^ ( (uint64_t) comp << 40 ) ^ (uint64_t) cacheAddr;
  m_uniqAll.insert( key ); m_rdAll.access( key );
  if ( isA ) { m_uniqA.insert( key ); m_rdA.access( key ); if ( m_subDerive ) m_subDerive->rawAccessResolved( cacheAddr, poc, comp ); }
  if ( isB ) { m_uniqB.insert( key ); m_rdB.access( key ); if ( m_subRecon )  m_subRecon->rawAccessResolved( cacheAddr, poc, comp ); }
  const int sp = m_refPoc; const ComponentID sc = m_compID; m_refPoc = poc; m_compID = comp;
  const bool hit = xAccessCore( cacheAddr );
  m_refPoc = sp; m_compID = sc;
  if ( !hit ) m_siteMiss[ site ]++;
}

void CacheModel::xRegisterPicture( const Picture* pic, bool isCur )
{
  if ( !pic ) return;
  for ( int c = 0; c < getNumberValidComponents( pic->chromaFormat ); c++ )
  {
    const ComponentID comp = (ComponentID) c;
    const Pel* base = pic->getOrigin( PIC_RECONSTRUCTION, comp );
    bool known = false; for ( auto &b : m_bufs ) if ( b.base == base ) { known = true; b.isCur = isCur; b.poc = pic->getPOC(); break; }
    if ( known ) continue;
    const CPelBuf rb = pic->getRecoBuf( comp );
    const int ymargin = c == 0 ? (int) pic->margin : (int) ( pic->margin >> getComponentScaleY( comp, pic->chromaFormat ) );
    BufReg r; r.base = base; r.stride = rb.stride; r.extent = (size_t) rb.stride * ( rb.height + 2 * ymargin ); r.poc = pic->getPOC(); r.comp = c; r.isCur = isCur;
    m_bufs.push_back( r );
  }
}

const CacheModel::BufReg* CacheModel::xResolve( const Pel* addr )
{
  if ( g_j0090Pu && g_j0090Pu->cs->picture != m_curPic )       // new current picture: refresh flags (yesterday's current picture is today's reference)
  {
    m_curPic = g_j0090Pu->cs->picture;
    for ( auto &b : m_bufs ) b.isCur = false;
    const Slice* slice = g_j0090Pu->cu->slice;
    for ( int l = 0; l < 2; l++ ) for ( int i = 0; i < slice->getNumRefIdx( (RefPicList) l ); i++ ) xRegisterPicture( slice->getRefPic( (RefPicList) l, i ), false );
    xRegisterPicture( m_curPic, true );
  }
  for ( int pass = 0; pass < 2; pass++ )
  {
    for ( auto &b : m_bufs ) if ( addr >= b.base && addr < b.base + b.extent ) return &b;
    if ( pass == 0 && g_j0090Pu )     // lazily register the current slice's reference pictures and the current picture
    {
      const Slice* slice = g_j0090Pu->cu->slice;
      for ( int l = 0; l < 2; l++ ) for ( int i = 0; i < slice->getNumRefIdx( (RefPicList) l ); i++ ) xRegisterPicture( slice->getRefPic( (RefPicList) l, i ), false );
      xRegisterPicture( g_j0090Pu->cs->picture, true );
    }
    else break;
  }
  return nullptr;
}

void CacheModel::rawAccessResolved( size_t cacheAddr, int poc, ComponentID comp )
{
  if ( !m_cacheEnable ) return;
  const int sp = m_refPoc; const ComponentID sc = m_compID; m_refPoc = poc; m_compID = comp;
  xAccessCore( cacheAddr );
  m_refPoc = sp; m_compID = sc;
}

void CacheModel::rawAccess( const Pel *addr )
{
  if ( !m_cacheEnable ) return;
  xAccessCore( xMapAddress( (size_t) (addr - m_base) ) >> m_shift );
}

bool CacheModel::xAccessCore( size_t cacheAddr )
{
  bool hit = false;
  int  entry = (int) (cacheAddr % m_numCacheLine);
  int  pos   = entry * m_numWay;
  int  way;
  for ( way = 0 ; way < m_numWay ; way++ )
  {
      if ( xIsCacheHit( pos + way, cacheAddr ) ) {
        hit = true;
        break;
      }
  }
  if ( !hit )
  {
    m_missHitCount++;
    xUpdateCache( entry, cacheAddr );
  }
  else
  {
    m_hitCount[ pos + way ] ++;
    xUpdateCacheStatus( entry, way );
  }
  m_totalAccess++;
  return hit;
}

void CacheModel::setCacheEnable( bool enable )
{
  m_cacheEnableFilter = enable;
}

void CacheModel::setTmpCacheEnable(bool enable)
{
  m_cacheEnableFilterTmp.push(m_cacheEnableFilter);
  m_cacheEnableFilter = enable;
}

void CacheModel::restoreCacheEnable()
{
  m_cacheEnableFilter = m_cacheEnableFilterTmp.top();
  m_cacheEnableFilterTmp.pop();
}

#endif // JVET_J0090_MEMORY_BANDWITH_MEASURE
