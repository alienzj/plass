//
// Created by Martin Steinegger on 2/25/16.
//

#include <limits>
#include <string>
#include <vector>
#include <iomanip>
#include <algorithm>
#include "Indexer.h"
#include "ReducedMatrix.h"
#include "DBWriter.h"
#include "SubstitutionMatrix.h"
#include "Util.h"
#include "Parameters.h"
#include "Matcher.h"
#include "Debug.h"
#include "DBReader.h"
#include "omptl/omptl_algorithm"
#include <sys/time.h>
#include "MathUtil.h"
#include "FileUtil.h"
#include <tantan.h>
#include <unordered_map>
#include "QueryMatcher.h"
#include "FileUtil.h"

#ifdef OPENMP
#include <omp.h>
#endif

#ifndef SIZE_T_MAX
#define SIZE_T_MAX ((size_t) -1)
#endif
struct KmerPosition {
    size_t kmer;
    unsigned int id;
    unsigned short seqLen;
    short pos;
    KmerPosition(){}
    KmerPosition(size_t kmer, unsigned int id, unsigned short seqLen, short pos):
    kmer(kmer), id(id), seqLen(seqLen), pos(pos) {}
    static bool compareRepSequenceAndIdAndPos(const KmerPosition &first, const KmerPosition &second){
        if(first.kmer < second.kmer )
            return true;
        if(second.kmer < first.kmer )
            return false;
        if(first.seqLen > second.seqLen )
            return true;
        if(second.seqLen > first.seqLen )
            return false;
        if(first.id < second.id )
            return true;
        if(second.id < first.id )
            return false;
        if(first.pos < second.pos )
            return true;
        if(second.pos < first.pos )
            return false;
        return false;
    }
    
    static bool compareRepSequenceAndIdAndDiag(const KmerPosition &first, const KmerPosition &second){
        if(first.kmer < second.kmer)
            return true;
        if(second.kmer < first.kmer)
            return false;
        if(first.id < second.id)
            return true;
        if(second.id < first.id)
            return false;
        
        //        const short firstDiag  = (first.pos < 0)  ? -first.pos : first.pos;
        //        const short secondDiag = (second.pos  < 0) ? -second.pos : second.pos;
        if(first.pos < second.pos)
            return true;
        if(second.pos < first.pos)
            return false;
        return false;
    }
};

struct SequencePosition{
    short score;
    size_t kmer;
    unsigned int pos;
    static bool compareByScore(const SequencePosition &first, const SequencePosition &second){
        if(first.score < second.score)
            return true;
        if(second.score < first.score)
            return false;
        if(first.kmer < second.kmer)
            return true;
        if(second.kmer < first.kmer)
            return false;
        
        return false;
    }
};

void setKmerLengthAndAlphabet(Parameters &parameters);

void writeKmersToDisk(std::string tmpFile, KmerPosition *kmers, size_t totalKmers);

std::pair<KmerPosition *, size_t> mergeKmerFiles(std::vector<std::string> tmpFiles);

struct __attribute__((__packed__)) KmerEntry {
    unsigned int seqId;
    short diagonal;
};

void mergeFilePair(const KmerPosition *entry1, size_t entrySize1, KmerEntry *entry2, size_t entrySize2,
                   std::vector<KmerPosition> & out);

void setLinearFilterDefault(Parameters *p) {
    p->spacedKmer = false;
    p->covThr = 0.8;
    p->kmerSize = Parameters::CLUST_LINEAR_DEFAULT_K;
    p->alphabetSize = Parameters::CLUST_LINEAR_DEFAULT_ALPH_SIZE;
    p->kmersPerSequence = 20;
}


size_t computeKmerCount(DBReader<unsigned int> &reader, size_t KMER_SIZE, size_t chooseTopKmer) {
    size_t totalKmers = 0;
    for(size_t id = 0; id < reader.getSize(); id++ ){
        int kmerAdjustedSeqLen = std::max(0, static_cast<int>(reader.getSeqLens(id) - 2 ) - static_cast<int>(KMER_SIZE ) + 1) ;
        totalKmers += std::min(kmerAdjustedSeqLen, static_cast<int>( chooseTopKmer ) );
    }
    return totalKmers;
}

size_t computeMemoryNeededLinearfilter(size_t totalKmer) {
    return sizeof(KmerPosition) * totalKmer;
}

#define RoL(val, numbits) (val << numbits) ^ (val >> (32 - numbits))
unsigned circ_hash(const int * x, unsigned length, const unsigned rol){
    short unsigned RAND[21] = {0x4567, 0x23c6, 0x9869, 0x4873, 0xdc51, 0x5cff, 0x944a, 0x58ec, 0x1f29, 0x7ccd, 0x58ba, 0xd7ab, 0x41f2, 0x1efb, 0xa9e3, 0xe146, 0x007c, 0x62c2, 0x0854, 0x27f8, 0x231b};
    short unsigned h = 0x0;
    h = h^ RAND[x[0]];                  // XOR h and ki
    for (unsigned int i = 1; i < length; ++i){
        h = RoL(h, rol);
        h ^= RAND[x[i]];                   // XOR h and ki
    }
    return h;
}

// Rolling hash for CRC variant: compute hash value for next key x[0:length-1] from previous hash value hash( x[-1:length-2] ) and x_first = x[-1]
unsigned circ_hash_next(const int * x, unsigned length, int x_first, short unsigned h, const unsigned rol){
    short unsigned RAND[21] = {0x4567, 0x23c6, 0x9869, 0x4873, 0xdc51, 0x5cff, 0x944a, 0x58ec, 0x1f29, 0x7ccd, 0x58ba, 0xd7ab, 0x41f2, 0x1efb, 0xa9e3, 0xe146, 0x007c, 0x62c2, 0x0854, 0x27f8, 0x231b};
    h ^= RoL(RAND[x_first], (5*(length-1)) % 16); // undo INITIAL_VALUE and first letter x[0] of old key
    h =  RoL(h, rol); // circularly permute all letters x[1:length-1] to 5 positions to left
    h ^= RAND[x[length-1]]; // add new, last letter of new key x[1:length]
    return h;
}
#undef RoL

size_t fillKmerPositionArray(KmerPosition * hashSeqPair, DBReader<unsigned int> &seqDbr,
                             Parameters & par, BaseMatrix * subMat,
                             size_t KMER_SIZE, size_t chooseTopKmer,
                             size_t splits, size_t split){
    size_t offset = 0;
    double probMatrix[subMat->alphabetSize][subMat->alphabetSize];
    const double *probMatrixPointers[subMat->alphabetSize];
    char hardMaskTable[256];
    if (par.maskMode == 0) {
        std::fill_n(hardMaskTable, 256, subMat->aa2int[(int) 'X']);
        for (int i = 0; i < subMat->alphabetSize; ++i) {
            probMatrixPointers[i] = probMatrix[i];
            for (int j = 0; j < subMat->alphabetSize; ++j) {
                probMatrix[i][j] = subMat->probMatrix[i][j] / (subMat->pBack[i] * subMat->pBack[j]);
            }
        }
    }
    
    
#pragma omp parallel
    {
        Sequence seq(par.maxSeqLen, subMat->aa2int, subMat->int2aa, Sequence::AMINO_ACIDS, KMER_SIZE, false, false);
        Indexer idxer(subMat->alphabetSize, KMER_SIZE);
        char * charSequence = new char[par.maxSeqLen];
        const unsigned int BUFFER_SIZE = 1024;
        size_t bufferPos = 0;
        KmerPosition * threadKmerBuffer = new KmerPosition[BUFFER_SIZE];
        SequencePosition * kmers = new SequencePosition[par.maxSeqLen];
#pragma omp for schedule(dynamic, 100)
        for(size_t id = 0; id < seqDbr.getSize(); id++){
            Debug::printProgress(id);
            seq.mapSequence(id, id, seqDbr.getData(id));
            
            // mask using tantan
            if (par.maskMode == 1) {
                for (int i = 0; i < seq.L; i++) {
                    charSequence[i] = (char) seq.int_sequence[i];
                }
                tantan::maskSequences(charSequence,
                                      charSequence + seq.L,
                                      50 /*options.maxCycleLength*/,
                                      probMatrixPointers,
                                      0.005 /*options.repeatProb*/,
                                      0.05 /*options.repeatEndProb*/,
                                      0.5 /*options.repeatOffsetProbDecay*/,
                                      0, 0,
                                      0.5 /*options.minMaskProb*/, hardMaskTable);
                for (int i = 0; i < seq.L; i++) {
                    seq.int_sequence[i] = charSequence[i];
                }
            }
            
            int seqKmerCount = 0;
            unsigned int seqId = seq.getId();
            unsigned short prevHash=0;
            unsigned int prevFirstRes = 0;
            if(seq.hasNextKmer()){
                const int *kmer = seq.nextKmer();
                prevHash = circ_hash(kmer, KMER_SIZE, par.hashShift);
                prevFirstRes = kmer[0];
            }
            while(seq.hasNextKmer()) {
                const int *kmer = seq.nextKmer();
                //float kmerScore = 1.0;
                prevHash = circ_hash_next(kmer, KMER_SIZE, prevFirstRes, prevHash, par.hashShift);
                prevFirstRes = kmer[0];
                size_t xCount = 0;
                for (size_t kpos = 0; kpos < KMER_SIZE; kpos++) {
                    xCount += (kmer[kpos] == subMat->aa2int[(int)'X']);
                }
                if(xCount > 0){
                    continue;
                }
                (kmers+seqKmerCount)->score = prevHash;
                size_t kmerIdx = idxer.int2index(kmer, 0, KMER_SIZE);
                (kmers+seqKmerCount)->kmer = kmerIdx;
                (kmers+seqKmerCount)->pos = seq.getCurrentPosition();
                seqKmerCount++;
            }
            if(seqKmerCount > 1){
                std::stable_sort(kmers, kmers+seqKmerCount, SequencePosition::compareByScore);
            }
            size_t kmerConsidered = std::min(static_cast<int>(chooseTopKmer), seqKmerCount);
            for(size_t topKmer = 0; topKmer < kmerConsidered; topKmer++){
                size_t splitIdx = (kmers + topKmer)->kmer % splits;
                if(splitIdx != split) {
                    continue;
                }
                threadKmerBuffer[bufferPos].kmer  = (kmers + topKmer)->kmer;
                threadKmerBuffer[bufferPos].id    = seqId;
                threadKmerBuffer[bufferPos].pos   = (kmers + topKmer)->pos;
                threadKmerBuffer[bufferPos].seqLen  = seq.L;
                bufferPos++;
                if(bufferPos >= BUFFER_SIZE){
                    size_t writeOffset = __sync_fetch_and_add(&offset, bufferPos);
                    memcpy(hashSeqPair+writeOffset, threadKmerBuffer, sizeof(KmerPosition) * bufferPos);
                    bufferPos = 0;
                }
            }
        }
        
        if(bufferPos > 0){
            size_t writeOffset = __sync_fetch_and_add(&offset, bufferPos);
            memcpy(hashSeqPair+writeOffset, threadKmerBuffer, sizeof(KmerPosition) * bufferPos);
        }
        delete [] kmers;
        delete [] charSequence;
        delete [] threadKmerBuffer;
    }
    return offset;
}


int kmermatcher(int argc, const char **argv, const Command &command) {
    Parameters &par = Parameters::getInstance();
    setLinearFilterDefault(&par);
    par.parseParameters(argc, argv, command, 2, true, false, MMseqsParameter::COMMAND_CLUSTLINEAR);
    
    if (par.maskMode == 2) {
        Debug(Debug::ERROR) << "kmermatcher does not support mask mode 2.\n";
        EXIT(EXIT_FAILURE);
    }
    
    setKmerLengthAndAlphabet(par);
#ifdef OPENMP
    omp_set_num_threads(par.threads);
#endif
    struct timeval start, end;
    gettimeofday(&start, NULL);
    
    BaseMatrix *subMat;
    if (par.alphabetSize == 21) {
        subMat = new SubstitutionMatrix(par.scoringMatrixFile.c_str(), 2.0, 0.0);
    } else {
        SubstitutionMatrix sMat(par.scoringMatrixFile.c_str(), 2.0, 0.0);
        subMat = new ReducedMatrix(sMat.probMatrix, sMat.subMatrixPseudoCounts, par.alphabetSize, 2.0);
    }
    
    DBReader<unsigned int> seqDbr(par.db1.c_str(), (par.db1 + ".index").c_str());
    seqDbr.open(DBReader<unsigned int>::NOSORT);
    //seqDbr.readMmapedDataInMemory();
    const size_t KMER_SIZE = par.kmerSize;
    size_t chooseTopKmer = par.kmersPerSequence;
    DBWriter dbw(par.db2.c_str(), std::string(par.db2 + ".index").c_str(), 1);
    dbw.open();
    size_t totalMemoryInByte = Util::getTotalSystemMemory();
    Debug(Debug::WARNING) << "Check requirements\n";
    size_t totalKmers = computeKmerCount(seqDbr, KMER_SIZE, chooseTopKmer);
    size_t totalSizeNeeded = computeMemoryNeededLinearfilter(totalKmers);
    Debug(Debug::INFO) << "Needed memory (" << totalSizeNeeded << " byte) of total memory (" << totalMemoryInByte
    << " byte)\n";
    // compute splits
    size_t splits = static_cast<int>(std::ceil(static_cast<float>(totalSizeNeeded)/ (static_cast<float>(totalMemoryInByte)*0.9)));
    if(splits > 1){
        splits += 1; //security buffer
    }
    Debug(Debug::WARNING) << "Process file into " << splits << " parts\n";
    Debug(Debug::WARNING) << "Generate k-mers list ... \n";
    std::vector<std::string> splitFiles;
    KmerPosition *hashSeqPair=NULL;
    
    for(size_t split = 0; split < splits; split++){
        size_t splitKmerCount = (splits > 1) ? static_cast<size_t >(static_cast<double>(totalKmers/splits) * 1.2) : totalKmers;
        Debug(Debug::INFO) << "Allocate " << splitKmerCount << " elements. Total memory " << computeMemoryNeededLinearfilter(splitKmerCount) << ".\n";
        hashSeqPair = new KmerPosition[splitKmerCount + 1];
        Util::checkAllocation(hashSeqPair, "Could not allocat memory");
#pragma omp parallel for
        for (size_t i = 0; i < splitKmerCount + 1; i++) {
            hashSeqPair[i].kmer = SIZE_T_MAX;
        }
        
        size_t elementsToSort = fillKmerPositionArray(hashSeqPair, seqDbr, par, subMat, KMER_SIZE, chooseTopKmer, splits, split);
        if(splits == 1){
            seqDbr.unmapData();
        }
        Debug(Debug::WARNING) << "Done." << "\n";
        Debug(Debug::WARNING) << "Sort kmer ... ";
        omptl::sort(hashSeqPair, hashSeqPair + elementsToSort, KmerPosition::compareRepSequenceAndIdAndPos);
        Debug(Debug::WARNING) << "Done." << "\n";
        // assign rep. sequence to same kmer members
        // The longest sequence is the first since we sorted by kmer, seq.Len and id
        size_t prevHash = hashSeqPair[0].kmer;
        size_t repSeqId = hashSeqPair[0].id;
        unsigned int repSeq_i_pos = hashSeqPair[0].pos;
        size_t prevHashStart = 0;
        size_t prevSetSize = 0;
        size_t elementIdx = 0;
        for (elementIdx = 0; elementIdx < splitKmerCount + 1; elementIdx++) {
            if (prevHash != hashSeqPair[elementIdx].kmer) {
                for (size_t i = prevHashStart; i < elementIdx; i++) {
                    hashSeqPair[i].kmer = (hashSeqPair[i].kmer != SIZE_T_MAX) ? ((prevSetSize == 1) ? SIZE_T_MAX : repSeqId)
                    : SIZE_T_MAX;
                    short diagonal = repSeq_i_pos - hashSeqPair[i].pos;
                    hashSeqPair[i].pos = diagonal;
                }
                prevSetSize = 0;
                prevHashStart = elementIdx;
                repSeqId = hashSeqPair[elementIdx].id;
                repSeq_i_pos = hashSeqPair[elementIdx].pos;
            }
            if (hashSeqPair[elementIdx].kmer == SIZE_T_MAX) {
                break;
            }
            prevSetSize++;
            prevHash = hashSeqPair[elementIdx].kmer;
        }
        // sort by rep. sequence (stored in kmer) and sequence id
        Debug(Debug::WARNING) << "Sort by rep. sequence ... ";
        omptl::sort(hashSeqPair, hashSeqPair + elementsToSort, KmerPosition::compareRepSequenceAndIdAndDiag);
        Debug(Debug::WARNING) << "Done\n";
        if(splits > 1){
            std::string splitFile = par.db2 + "_split_" +SSTR(split);
            splitFiles.push_back(splitFile);
            writeKmersToDisk(splitFile, hashSeqPair, splitKmerCount + 1);
            delete [] hashSeqPair;
        }
    }
    
    if(splits > 1){
        seqDbr.unmapData();
        std::pair<KmerPosition *, size_t> ret = mergeKmerFiles(splitFiles);
        totalKmers = ret.second;
        hashSeqPair = ret.first;
#pragma omp parallel for schedule(static)
        for(size_t pos = 0; pos < totalKmers; pos++){
            hashSeqPair[pos].seqLen = seqDbr.getSeqLens(hashSeqPair[pos].id) - 2;
        }
    }
    std::vector<bool> repSequence(seqDbr.getSize());
    std::fill(repSequence.begin(), repSequence.end(), false);
    size_t repSeqId = SIZE_T_MAX;
    size_t lastTargetId = SIZE_T_MAX;
    unsigned int writeSets = 0;
    unsigned int queryLength = 0;
    // write result
    std::string prefResultsOutString;
    prefResultsOutString.reserve(1000000000);
    char buffer[100];
    size_t kmerPos=0;
    size_t reduced=0;
    size_t members = 0;
    size_t repSeqs = 0;
    for(kmerPos = 0; kmerPos < totalKmers && hashSeqPair[kmerPos].kmer != SIZE_T_MAX; kmerPos++){
        if(repSeqId != hashSeqPair[kmerPos].kmer) {
            if (writeSets > 0) {
                repSequence[repSeqId] = true;
                dbw.writeData(prefResultsOutString.c_str(), prefResultsOutString.length(), seqDbr.getDbKey(repSeqId), 0);
            }else{
                if(repSeqId != SIZE_T_MAX) {
                    repSequence[repSeqId] = false;
                }
            }
            lastTargetId = SIZE_T_MAX;
            prefResultsOutString.clear();
            repSeqId = hashSeqPair[kmerPos].kmer;
            queryLength = hashSeqPair[kmerPos].seqLen;
            hit_t h;
            h.seqId = seqDbr.getDbKey(repSeqId);
            h.pScore = 0;
            h.diagonal = 0;
            int len = QueryMatcher::prefilterHitToBuffer(buffer, h);
            // TODO: error handling for len
            prefResultsOutString.append(buffer, len);
            repSeqs++;
        }
        unsigned int targetId = hashSeqPair[kmerPos].id;
        unsigned int targetLength = hashSeqPair[kmerPos].seqLen;
        unsigned short diagonal = hashSeqPair[kmerPos].pos;
        // remove similar double sequence hit
        if(targetId != repSeqId && lastTargetId != targetId ){
           if(par.covThr > 0.0 ) {
               if ((((float) queryLength) / ((float) targetLength) < par.covThr) ||
                    (((float) targetLength) / ((float) queryLength) < par.covThr)) {
                   lastTargetId = targetId;
                   continue;
               }
           }
        }else{
            reduced++;
            lastTargetId = targetId;
            continue;
        }
        hit_t h;
        h.seqId = seqDbr.getDbKey(targetId);
        h.pScore = 0;
        h.diagonal = diagonal;
        int len = QueryMatcher::prefilterHitToBuffer(buffer, h);
        prefResultsOutString.append(buffer, len);
        members++;
        lastTargetId = targetId;
        writeSets++;
    }
    if (writeSets > 0) {
        repSequence[repSeqId] = true;
        dbw.writeData(prefResultsOutString.c_str(), prefResultsOutString.length(), seqDbr.getDbKey(repSeqId), 0);
    }else{
        if(repSeqId != SIZE_T_MAX) {
            repSequence[repSeqId] = false;
        }
    }
    std::cout << kmerPos << " " << totalKmers << " " << repSeqs  << " " << reduced << " "  << members << std::endl;
    // add missing entries to the result (needed for clustering)
    hit_t h;
    h.pScore = 0;
    h.diagonal = 0;
    for(size_t id = 0; id < seqDbr.getSize(); id++){
        if(repSequence[id] == false){
            h.seqId =  seqDbr.getDbKey(id);
            int len = QueryMatcher::prefilterHitToBuffer(buffer, h);
            dbw.writeData(buffer, len, seqDbr.getDbKey(id), 0);
        }
    }
    // free memory
    delete subMat;
    delete [] hashSeqPair;
    seqDbr.close();
    dbw.close();
    
    gettimeofday(&end, NULL);
    time_t sec = end.tv_sec - start.tv_sec;
    Debug(Debug::WARNING) << "Time for processing: " << (sec / 3600) << " h " << (sec % 3600 / 60) << " m " << (sec % 60) << "s\n";
    return 0;
}


class CompareKmerEntryBySeqId {
public:
    bool operator() (const KmerEntry & r1,const KmerEntry & r2) {
        if(r1.seqId > r2.seqId )
            return true;
        if(r2.seqId > r1.seqId )
            return false;
        /*  int seqLen1 = r1.qEndPos - r1.qStartPos;
         int seqLen2 = r2.qEndPos - r2.qStartPos;
         if(seqLen1 < seqLen2)
         return true;
         if(seqLen2 < seqLen1 )
         return false;*/
        return false;
    }
};

std::pair<KmerPosition *, size_t> mergeKmerFiles(std::vector<std::string> tmpFiles) {
    size_t entrySize1 = FileUtil::getFileSize(tmpFiles[0])/sizeof(KmerEntry);
    FILE * file1   = FileUtil::openFileOrDie(tmpFiles[0].c_str(), "rb",true);
    KmerEntry * entry1 = new KmerEntry[entrySize1];
    size_t readSize1 = fread(entry1, sizeof(KmerEntry), entrySize1, file1);
    if(readSize1 != entrySize1){
        Debug(Debug::ERROR) << "Error reading " << tmpFiles[0] << " readSize1 "
        << readSize1 << " != entrySize1 " << entrySize1 << "\n";
        EXIT(EXIT_FAILURE);
    }
    size_t repSeq = entry1[0].seqId;
    std::vector<KmerPosition> data1;
    data1.reserve(entrySize1);
    for(size_t pos1 = 0; pos1 < entrySize1; pos1++ ){
        if(entry1[pos1].seqId == UINT_MAX){
            if(pos1+1 < entrySize1){
                repSeq = entry1[pos1+1].seqId;
            }
            continue;
        }
        unsigned int id = entry1[pos1].seqId;
        short diagonal = entry1[pos1].diagonal;
        data1.emplace_back(repSeq, id, 0, diagonal);
    }
    std::vector<KmerPosition> data2;
    data2.reserve(entrySize1);
    for(size_t fileIdx = 1; fileIdx < tmpFiles.size(); fileIdx++){
        FILE * file2   = FileUtil::openFileOrDie(tmpFiles[fileIdx].c_str(), "rb",true);
        size_t entrySize2 = FileUtil::getFileSize(tmpFiles[fileIdx])/sizeof(KmerEntry);
        KmerEntry * entry2 = new KmerEntry[entrySize2];
        size_t readSize2 = fread(entry2, sizeof(KmerEntry), entrySize2, file2);
        if(readSize2 != entrySize2){
            Debug(Debug::ERROR) << "Error reading "<< tmpFiles[fileIdx] << " entrySize2 "
            << entrySize2 <<" != entrySize2 " << entrySize2 << "\n";
            EXIT(EXIT_FAILURE);
        }
        mergeFilePair(data1.data(), data1.size(), entry2, entrySize2, data2);
        std::vector<KmerPosition> tmp = data1;
        data1 = data2;
        data2 = tmp;
        data2.clear();
        fclose(file2);
        delete [] entry2;
    }
    data2.clear();
    size_t returnSize = data1.size();
    KmerPosition * returnData = new KmerPosition[returnSize+1];
    std::copy(data1.begin(), data1.end(), returnData);
    data1.size();
    returnData[returnSize].id=SIZE_MAX;
    return std::make_pair(returnData, returnSize);
}

void mergeFilePair(const KmerPosition *entry1, size_t entrySize1, KmerEntry *entry2, size_t entrySize2,
                   std::vector<KmerPosition> & out) {
    size_t i = 0, j = 0;
    unsigned int repSeq2 = entry2[0].seqId;
    while ( i < entrySize1 && j < entrySize2 - 1) // last element of entry2 is UINT_MAX
    {
        // remove duplicate elements in group
        if (entry1[i].kmer == repSeq2){
            size_t id1 = entry1[i].kmer;
            while(i < entrySize1 && entry1[i].kmer == id1 && entry2[j].seqId != UINT_MAX){
                if(entry1[i].id == entry2[j].seqId){
                    if(entry1[i].pos <= entry2[j].diagonal ){
                        out.push_back(entry1[i]);
                    }else{
                        unsigned int id = entry2[j].seqId;
                        short diagonal  = entry2[j].diagonal;
                        out.emplace_back(repSeq2, id, 0, diagonal);
                    }
                    i++;
                    j++;
                } else if(entry1[i].id < entry2[j].seqId){
                    out.push_back(entry1[i]);
                    i++;
                }else{
                    unsigned int id = entry2[j].seqId;
                    short diagonal  = entry2[j].diagonal;
                    out.emplace_back(repSeq2, id, 0, diagonal);
                    j++;
                }
            }
            
            
            while (i < entrySize1 && entry1[i].kmer == id1){
                out.push_back(entry1[i]);
                i++;
            }
            while (entry2[j].seqId != UINT_MAX){
                unsigned int id = entry2[j].seqId;
                short diagonal  = entry2[j].diagonal;
                out.emplace_back(repSeq2, id, 0, diagonal);
                j++;
            }
            if(entry2[j].seqId==UINT_MAX){
                if(j+1 < entrySize2){
                    j++;
                    repSeq2 = entry2[j].seqId;
                }
            }
        }else if (entry1[i].kmer < repSeq2){
            out.push_back(entry1[i++]);
        }else{
            unsigned int id = entry2[j].seqId;
            short diagonal  = entry2[j].diagonal;
            out.emplace_back(repSeq2, id, 0, diagonal);
            j++;
            if(entry2[j].seqId==UINT_MAX){
                if(j+1 < entrySize2){
                    j++;
                    repSeq2 = entry2[j].seqId;
                }
            }
        }
    }
    while (i < entrySize1){
        out.push_back(entry1[i]);
        i++;
    }
    while (j < entrySize2 - 1){ // last element is UINT_MAX
        unsigned int id = entry2[j].seqId;
        short diagonal  = entry2[j].diagonal;
        out.emplace_back(repSeq2, id, 0, diagonal); 
        j++;
        if(entry2[j].seqId==UINT_MAX){
            if(j+1 < entrySize2){
                j++;
                repSeq2 = entry2[j].seqId;
            }else{
                break;
            }
        }
    }
}


void writeKmersToDisk(std::string tmpFile, KmerPosition *hashSeqPair, size_t totalKmers) {
    size_t repSeqId = SIZE_T_MAX;
    size_t lastTargetId = SIZE_T_MAX;
    FILE* filePtr = fopen(tmpFile.c_str(), "wb");
    if(filePtr == NULL) { perror(tmpFile.c_str()); EXIT(EXIT_FAILURE); }
    unsigned int writeSets = 0;
    const size_t BUFFER_SIZE = 2048;
    size_t bufferPos = 0;
    KmerEntry writeBuffer[BUFFER_SIZE];
    KmerEntry nullEntry;
    nullEntry.seqId=UINT_MAX;
    nullEntry.diagonal=0;
    for(size_t kmerPos = 0; kmerPos < totalKmers && hashSeqPair[kmerPos].kmer != SIZE_T_MAX; kmerPos++){
        if(repSeqId != hashSeqPair[kmerPos].kmer) {
            if (writeSets > 0 ) {
                if(bufferPos > 0){
                    fwrite(writeBuffer, sizeof(KmerEntry), bufferPos, filePtr);
                }
                fwrite(&nullEntry, sizeof(KmerEntry), 1, filePtr);
            }
            lastTargetId = SIZE_T_MAX;
            bufferPos=0;
            repSeqId = hashSeqPair[kmerPos].kmer;
            writeBuffer[bufferPos].seqId = repSeqId;
            writeBuffer[bufferPos].diagonal = 0;
            bufferPos++;
        }
        unsigned int targetId = hashSeqPair[kmerPos].id;
        unsigned short diagonal = hashSeqPair[kmerPos].pos;
        // remove similar double sequence hit
        if(targetId != repSeqId && lastTargetId != targetId ){
            ;
        }else{
            lastTargetId = targetId;
            continue;
        }
        writeBuffer[bufferPos].seqId = targetId;
        writeBuffer[bufferPos].diagonal = diagonal;
        bufferPos++;
        if(bufferPos >= BUFFER_SIZE){
            fwrite(writeBuffer, sizeof(KmerEntry), bufferPos, filePtr);
            bufferPos=0;
        }
        lastTargetId = targetId;
        writeSets++;
    }
    if (writeSets > 0 && bufferPos > 0) {
        fwrite(writeBuffer, sizeof(KmerEntry), bufferPos, filePtr);
        fwrite(&nullEntry,  sizeof(KmerEntry), 1, filePtr);
    }
    fclose(filePtr);
}

void setKmerLengthAndAlphabet(Parameters &parameters) {
    if(parameters.kmerSize == 0){
        if((parameters.seqIdThr+0.001)>=0.9){
            parameters.kmerSize = 14;
            parameters.alphabetSize = 13;
        }else{
            parameters.kmerSize = 10;
            parameters.alphabetSize = 13;
        }
    }
    Debug(Debug::WARNING) << "Alphabet size: " << parameters.alphabetSize;
    Debug(Debug::WARNING) << " k-mer size: " << parameters.kmerSize << "\n";
}

#undef SIZE_T_MAX


