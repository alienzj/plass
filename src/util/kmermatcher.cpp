//
// Created by Martin Steinegger on 2/25/16.
//

#include <limits>
#include <string>
#include <vector>
#include <iomanip>
#include <algorithm>
#include <mutex>
#include <Indexer.h>
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
#include <MathUtil.h>
#include <tantan.h>
#include <QueryMatcher.h>

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

    static bool compareKmerPositionByKmer(KmerPosition first, KmerPosition second){
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
        return false;
    }

    static bool compareRepSequenceAndId(KmerPosition first, KmerPosition second){
        if(first.kmer < second.kmer )
            return true;
        if(second.kmer < first.kmer )
            return false;
        if(first.id < second.id )
            return true;
        if(second.id < first.id )
            return false;
        return false;
    }

    static bool compareRepSequenceAndIdAndPos(KmerPosition first, KmerPosition second){
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

        const short firstDiag  = (first.pos < 0)  ? -first.pos : first.pos;
        const short secondDiag = (second.pos  < 0) ? -second.pos : second.pos;
        if(firstDiag < secondDiag)
            return true;
        if(secondDiag < firstDiag)
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
        if(first.pos < second.pos)
            return true;
        if(second.pos < first.pos)
            return false;
        return false;
    }
};

void setKmerLengthAndAlphabet(Parameters &parameters);

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
        int kmerAdjustedSeqLen = std::max(0, static_cast<int>(reader.getSeqLens(id)) - static_cast<int>(KMER_SIZE - 1)-2) ;
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
                             size_t KMER_SIZE, size_t chooseTopKmer){
    size_t kmerCounter = 0;
    double probMatrix[subMat->alphabetSize][subMat->alphabetSize];
    const double *probMatrixPointers[subMat->alphabetSize];
    char hardMaskTable[256];
    std::fill_n(hardMaskTable, 256, subMat->aa2int[(int)'X']);
    for (int i = 0; i < subMat->alphabetSize; ++i){
        probMatrixPointers[i] = probMatrix[i];
        for(int j = 0; j < subMat->alphabetSize; ++j){
            probMatrix[i][j]  = subMat->probMatrix[i][j]/(subMat->pBack[i]*subMat->pBack[j]);
        }
    }
#pragma omp parallel reduction (+: kmerCounter)
    {
        Sequence seq(par.maxSeqLen, subMat->aa2int, subMat->int2aa, Sequence::AMINO_ACIDS, KMER_SIZE, false, false);
        Indexer idxer(subMat->alphabetSize, KMER_SIZE);
        char * charSequence = new char[par.maxSeqLen];

        size_t dbFrom, dbSize;
        int thread_idx = 0;
#ifdef OPENMP
        thread_idx = omp_get_thread_num();
#endif
        Util::decomposeDomainByAminoAcid(seqDbr.getAminoAcidDBSize(), seqDbr.getSeqLens(), seqDbr.getSize(),
                                         thread_idx, par.threads, &dbFrom, &dbSize);
        size_t kmerStartPos = 0;
        for(size_t id = 0; id < dbFrom; id++ ){
            int kmerAdjustedSeqLen = std::max(0, static_cast<int>(seqDbr.getSeqLens(id)) - static_cast<int>(KMER_SIZE)  - 2);
            kmerStartPos += std::min(kmerAdjustedSeqLen, static_cast<int>(chooseTopKmer));
        }
        KmerPosition * tmpHashSeqPair=hashSeqPair + kmerStartPos;
        SequencePosition * kmers = new SequencePosition[par.maxSeqLen];
        for(size_t id = dbFrom; id < (dbFrom + dbSize); id++){
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
//                if(seqDbr.getDbKey(seqId)==102569||seqDbr.getDbKey(seqId)==404361){
//                    std::cout << kmerIdx << " " <<  seqDbr.getDbKey(seqId)<< std::endl;
//                }
                (kmers+seqKmerCount)->kmer = kmerIdx;
                (kmers+seqKmerCount)->pos = seq.getCurrentPosition();
                seqKmerCount++;
            }
            if(seqKmerCount > 1){
                std::sort(kmers, kmers+seqKmerCount, SequencePosition::compareByScore);
            }
            size_t kmerConsidered = std::min(static_cast<int>(chooseTopKmer), seqKmerCount);
            for(size_t topKmer = 0; topKmer < kmerConsidered; topKmer++){
                tmpHashSeqPair->kmer  = (kmers + topKmer)->kmer;
                tmpHashSeqPair->id    = seqId;
//                if(seqDbr.getDbKey(seqId)==102569||seqDbr.getDbKey(seqId)==402607){
//                    std::cout << tmpHashSeqPair->kmer << " " <<  seqDbr.getDbKey(seqId)<< std::endl;
//                }
                tmpHashSeqPair->pos   = (kmers + topKmer)->pos;
                tmpHashSeqPair->seqLen  = seq.L;
                tmpHashSeqPair++;
            }
            kmerCounter += kmerConsidered;
        }
        delete [] kmers;
        delete [] charSequence;
    }
    return kmerCounter;
}


int kmermatcher(int argc, const char **argv, const Command &command) {
    Parameters& par = Parameters::getInstance();
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

    BaseMatrix * subMat;
    if(par.alphabetSize == 21){
        subMat = new SubstitutionMatrix(par.scoringMatrixFile.c_str(), 2.0, 0.0);
    }else{
        SubstitutionMatrix sMat(par.scoringMatrixFile.c_str(), 2.0, 0.0);
        subMat = new ReducedMatrix(sMat.probMatrix, sMat.subMatrixPseudoCounts, par.alphabetSize, 2.0);
    }

    DBReader<unsigned int> seqDbr(par.db1.c_str(), (par.db1 + ".index").c_str());
    seqDbr.open(DBReader<unsigned int>::NOSORT);
    seqDbr.readMmapedDataInMemory();
    const size_t KMER_SIZE = par.kmerSize;
    size_t chooseTopKmer = par.kmersPerSequence;
    DBWriter dbw(par.db2.c_str(), std::string(par.db2 + ".index").c_str(), 1);
    dbw.open();
    size_t totalMemoryInByte =  Util::getTotalSystemMemory();
    Debug(Debug::WARNING) << "Check requirements\n";
    size_t totalKmers = computeKmerCount(seqDbr, KMER_SIZE, chooseTopKmer);
    size_t totalSizeNeeded = computeMemoryNeededLinearfilter(totalKmers);
    Debug(Debug::INFO) << "Needed memory (" << totalSizeNeeded << " byte) of total memory (" << totalMemoryInByte << " byte)\n";
    while(totalSizeNeeded > totalMemoryInByte){
        Debug(Debug::INFO) << "Adjust memory demand by reducing k-mers per sequence\n";
        chooseTopKmer = chooseTopKmer / 2;
        totalKmers = computeKmerCount(seqDbr, KMER_SIZE, chooseTopKmer);
        totalSizeNeeded = computeMemoryNeededLinearfilter(totalKmers);
        if(chooseTopKmer == 1 && totalSizeNeeded > totalMemoryInByte){
            Debug(Debug::ERROR) << "There is not enough memory on this machine\n";
            EXIT(EXIT_FAILURE);
        }
        Debug(Debug::INFO) << chooseTopKmer << " k-mer per sequence needs " << totalSizeNeeded << " byte of memory.\n";
    }
    Debug(Debug::WARNING) << "Generate k-mers list ... \n";
    KmerPosition * hashSeqPair = new KmerPosition[totalKmers+1];
#pragma omp parallel for
    for(size_t i = 0; i < totalKmers+1; i++){
        hashSeqPair[i].kmer = SIZE_T_MAX;
    }

    fillKmerPositionArray(hashSeqPair, seqDbr, par, subMat, KMER_SIZE, chooseTopKmer);
    seqDbr.unmapData();
    Debug(Debug::WARNING) << "Done." << "\n";
    Debug(Debug::WARNING) << "Sort kmer ... ";
    omptl::sort(hashSeqPair, hashSeqPair + totalKmers, KmerPosition::compareRepSequenceAndIdAndPos);
    Debug(Debug::WARNING) << "Done." << "\n";
    // assign rep. sequence to same kmer members
    // The longest sequence is the first since we sorted by kmer, seq.Len and id
    size_t prevHash = hashSeqPair[0].kmer;
    unsigned int queryId = hashSeqPair[0].id;
    unsigned int max_i_Pos = hashSeqPair[0].pos;
    size_t prevHashStart = 0;
    size_t prevSetSize = 0;

    for(size_t elementCount = 0; elementCount < totalKmers + 1; elementCount++) {

        if(prevHash != hashSeqPair[elementCount].kmer) {
            for(size_t i = prevHashStart; i < elementCount; i++){
                const unsigned int j_pos = hashSeqPair[i].pos;
//                if(hashSeqPair[i].kmer == 2824800637659372){
//                    std::cout  << hashSeqPair[i].kmer << " " <<  seqDbr.getDbKey(queryId) << " " <<  seqDbr.getDbKey(hashSeqPair[i].id)<< std::endl;
//                }
//                if(seqDbr.getDbKey(queryId) == 102569 && seqDbr.getDbKey(hashSeqPair[i].id) == 402607){
//                    std::cout << "bla" << std::endl;
//                }
                hashSeqPair[i].kmer = (hashSeqPair[i].kmer != SIZE_T_MAX) ? ((prevSetSize == 1 ) ? SIZE_T_MAX : queryId) : SIZE_T_MAX;
                short diagonal = max_i_Pos - j_pos;
                hashSeqPair[i].pos = diagonal;

            }
            prevSetSize = 0;
            prevHashStart = elementCount;
            queryId = hashSeqPair[elementCount].id;
            max_i_Pos = hashSeqPair[elementCount].pos;
        }
        prevSetSize++;
        prevHash = hashSeqPair[elementCount].kmer;

    }
    std::vector<bool> repSequence(seqDbr.getSize());
    std::fill(repSequence.begin(), repSequence.end(), false);

    // sort by rep. sequence (stored in kmer) and sequence id
    Debug(Debug::WARNING) << "Sort by rep. sequence ... ";
    omptl::sort(hashSeqPair, hashSeqPair + totalKmers, KmerPosition::compareRepSequenceAndIdAndDiag);
    Debug(Debug::WARNING) << "Done\n";

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

//        if(seqDbr.getDbKey(hashSeqPair[kmerPos].id)==32408||seqDbr.getDbKey(hashSeqPair[kmerPos].id)== 102569|| seqDbr.getDbKey(hashSeqPair[kmerPos].id)==404361){
//            std::cout  << hashSeqPair[kmerPos].kmer << " " <<  seqDbr.getDbKey(hashSeqPair[kmerPos].id)<< std::endl;
//        }
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
//        if(seqDbr.getDbKey(repSeqId) == 102569 ){
//            std::cout << "bla" << std::endl;
//        }
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

