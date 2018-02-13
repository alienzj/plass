#include <iomanip>
#include <itoa.h>
#include "Matcher.h"
#include "Util.h"
#include "Parameters.h"
#include "StripedSmithWaterman.h"

Matcher::Matcher(int querySeqType, int maxSeqLen, BaseMatrix *m, EvalueComputation * evaluer,
                 bool aaBiasCorrection, int gapOpen, int gapExtend){
    this->m = m;
    this->tinySubMat = NULL;
    this->gapOpen = gapOpen;
    this->gapExtend = gapExtend;
    if(querySeqType != Sequence::PROFILE_STATE_PROFILE ) {
        setSubstitutionMatrix(m);
    }

    this->maxSeqLen = maxSeqLen;
    nuclaligner=NULL;
    if(querySeqType==Sequence::NUCLEOTIDES){
        nuclaligner = new  BandedNucleotideAligner(m, maxSeqLen, gapOpen, gapExtend);
    }else{
        aligner = new SmithWaterman(maxSeqLen, m->alphabetSize, aaBiasCorrection);
    }
    this->evaluer = evaluer;
    //std::cout << "lambda=" << lambdaLog2 << " logKLog2=" << logKLog2 << std::endl;
}


void Matcher::setSubstitutionMatrix(BaseMatrix *m){
    this->tinySubMat = new int8_t[m->alphabetSize*m->alphabetSize];
    for (int i = 0; i < m->alphabetSize; i++) {
        for (int j = 0; j < m->alphabetSize; j++) {
            tinySubMat[i*m->alphabetSize + j] = m->subMatrix[i][j];
        }
    }
}

Matcher::~Matcher(){
    delete aligner;
    if(nuclaligner != NULL){
        delete nuclaligner;
    }
    if(tinySubMat != NULL){
        delete [] tinySubMat;
        tinySubMat = NULL;
    }
}

void Matcher::initQuery(Sequence* query){
    currentQuery = query;
    if(query->getSequenceType()==Sequence::NUCLEOTIDES){
        nuclaligner->initQuery(query);
    }else if(query->getSeqType() == Sequence::HMM_PROFILE || query->getSeqType() == Sequence::PROFILE_STATE_PROFILE){
        aligner->ssw_init(query, query->getAlignmentProfile(), this->m, this->m->alphabetSize, 2);
    }else{
        aligner->ssw_init(query, this->tinySubMat, this->m, this->m->alphabetSize, 2);
    }
}


Matcher::result_t Matcher::getSWResult(Sequence* dbSeq, const int diagonal, const int covMode, const float covThr,
                                       const double evalThr, const unsigned int mode, bool isIdentity){
    // calculation of the score and traceback of the alignment
    int32_t maskLen = currentQuery->L / 2;

    // calcuate stop score
//    const double qL = static_cast<double>(currentQuery->L);
//    const double dbL = static_cast<double>(dbSeq->L);

    // avoid nummerical issues -log(evalThr/(qL*dbL*seqDbSize))
//    double datapoints = -log(static_cast<double>(seqDbSize)) - log(qL) - log(dbL) + log(evalThr);
    //std::cout << seqDbSize << " " << 100 << " " << scoreThr << std::endl;
    //std::cout <<datapoints << " " << m->getBitFactor() <<" "<< evalThr << " " << seqDbSize << " " << currentQuery->L << " " << dbSeq->L<< " " << scoreThr << " " << std::endl;
    s_align alignment;
    if(dbSeq->getSequenceType()==Sequence::NUCLEOTIDES){
        if(diagonal==INT_MAX){
            Debug(Debug::ERROR) << "ERROR: Query sequence " << currentQuery->getDbKey()
                                << " has a result with no proper diagonal information , "
                                << "Please check your database.\n";
            EXIT(EXIT_FAILURE);
        }
        alignment = nuclaligner->align(dbSeq,diagonal,evaluer);
    }else if(isIdentity==false){
        alignment = aligner->ssw_align(dbSeq->int_sequence, dbSeq->L, gapOpen, gapExtend, mode, evalThr, evaluer, covMode, covThr, maskLen);
    }else{
        alignment = aligner->scoreIdentical(dbSeq->int_sequence, dbSeq->L, evaluer, mode);
    }

    // calculation of the coverage and e-value
    float qcov = 0.0;
    float dbcov = 0.0;
    float seqId = 0.0;
    // compute sequence identity
    std::string backtrace;

    int aaIds = 0;
    if(mode == Matcher::SCORE_COV_SEQID){
        if(isIdentity==false){
            if(alignment.cigar){
                int32_t targetPos = alignment.dbStartPos1, queryPos = alignment.qStartPos1;
                for (int32_t c = 0; c < alignment.cigarLen; ++c) {
                    char letter = SmithWaterman::cigar_int_to_op(alignment.cigar[c]);
                    uint32_t length = SmithWaterman::cigar_int_to_len(alignment.cigar[c]);
                    backtrace.reserve(length);

                    for (uint32_t i = 0; i < length; ++i){
                        if (letter == 'M') {
                            if (dbSeq->int_sequence[targetPos] == currentQuery->int_sequence[queryPos]){
                                aaIds++;
                            }
                            ++queryPos;
                            ++targetPos;
                            backtrace.append("M");
                        } else {
                            if (letter == 'I') {
                                ++queryPos;
                                backtrace.append("I");
                            }
                            else{
                                ++targetPos;
                                backtrace.append("D");
                            }
                        }
                    }
                }
            }
        } else {
            for (int32_t c = 0; c < alignment.cigarLen; ++c) {
                aaIds++;
                backtrace.append("M");
            }
        }
    }

    const unsigned int qStartPos = alignment.qStartPos1;
    const unsigned int dbStartPos = alignment.dbStartPos1;
    const unsigned int qEndPos = alignment.qEndPos1;
    const unsigned int dbEndPos = alignment.dbEndPos1;
    // normalize score
//    alignment->score1 = alignment->score1 - log2(dbSeq->L);
    if(mode == Matcher::SCORE_COV || mode == Matcher::SCORE_COV_SEQID) {
        qcov  = alignment.qCov;
        dbcov = alignment.tCov;
    }

    unsigned int alnLength = Matcher::computeAlnLength(qStartPos, qEndPos, dbStartPos, dbEndPos);
    // try to estimate sequence id
    if(mode == Matcher::SCORE_COV_SEQID){
        // compute sequence id
        unsigned int qAlnLen = std::max(qEndPos - qStartPos, static_cast<unsigned int>(1));
        unsigned int dbAlnLen = std::max(dbEndPos - dbStartPos, static_cast<unsigned int>(1));
        if(alignment.cigar){
            // OVERWRITE alnLength with gapped value
            alnLength = backtrace.size();
        }
        seqId =  static_cast<float>(aaIds) / static_cast<float>(std::max(std::max(qAlnLen, dbAlnLen), alnLength));
    }else if( mode == Matcher::SCORE_COV){
        // "20%   30%   40%   50%   60%   70%   80%   90%   99%"
        // "0.52  1.12  1.73  2.33  2.93  3.53  4.14  4.74  5.28"
        unsigned int qAlnLen = std::max(qEndPos - qStartPos, static_cast<unsigned int>(1));
        unsigned int dbAlnLen = std::max(dbEndPos - dbStartPos, static_cast<unsigned int>(1));
        //seqId = (alignment.score1 / static_cast<float>(std::max(qAlnLength, dbAlnLength)))  * 0.1656 + 0.1141;
        seqId = estimateSeqIdByScorePerCol(alignment.score1, qAlnLen, dbAlnLen);
    }else if ( mode == Matcher::SCORE_ONLY){
        unsigned int qAlnLen = std::max(qEndPos, static_cast<unsigned int>(1));
        unsigned int dbAlnLen = std::max(dbEndPos, static_cast<unsigned int>(1));
        //seqId = (alignment.score1 / static_cast<float>(std::max(dbAlnLen, qAlnLen)))  * 0.1656 + 0.1141;
        seqId = estimateSeqIdByScorePerCol(alignment.score1, qAlnLen, dbAlnLen);
    }

    //  E =  qL dL * exp^(-S/lambda)
    double evalue = alignment.evalue;
    int bitScore = static_cast<short>(evaluer->computeBitScore(alignment.score1)+0.5);

    result_t result(dbSeq->getDbKey(), bitScore, qcov, dbcov, seqId, evalue, alnLength, qStartPos, qEndPos, currentQuery->L, dbStartPos, dbEndPos, dbSeq->L, backtrace);
    delete [] alignment.cigar;
    return result;
}


std::vector<Matcher::result_t> Matcher::readAlignmentResults(char *data, bool readCompressed) {
    std::vector<Matcher::result_t> ret;
    if(data != NULL){
        while(*data != '\0'){
            Matcher::result_t result = parseAlignmentRecord(data, readCompressed);
            ret.push_back(result);
            data = Util::skipLine(data);
        }
    }
    return ret;
}

size_t Matcher::computeAlnLength(size_t qStart, size_t qEnd, size_t dbStart, size_t dbEnd) {
    return std::max(qEnd - qStart, dbEnd - dbStart) + 1;
}

float Matcher::estimateSeqIdByScorePerCol(uint16_t score, unsigned int qLen, unsigned int tLen) {
    float estimatedSeqId = (score / static_cast<float>(std::max(qLen, tLen))) * 0.1656 + 0.1141;
    estimatedSeqId = std::min(estimatedSeqId, 1.0f);
    return std::max(0.0f, estimatedSeqId);
}


std::string Matcher::compressAlignment(const std::string& bt) {
    std::string ret;
    char state = 'M';
    size_t counter = 0;
    for(size_t i = 0; i < bt.size(); i++){
        if(bt[i] != state){
            ret.append(std::to_string(counter));
            ret.push_back(state);
            state = bt[i];
            counter = 1;
        }else{
            counter++;
        }
    }
    ret.append(std::to_string(counter));
    ret.push_back(state);
    return ret;
}

std::string Matcher::uncompressAlignment(const std::string &cbt) {
    std::string bt;
    size_t count = 0;
    for(size_t i = 0; i < cbt.size(); i++) {
        sscanf(cbt.c_str() + i, "%zu", &count);
        for(size_t j = i; j < cbt.size(); j++ ){
            if(isdigit(cbt[j]) == false){
                char state = cbt[j];
                bt.append(count, state);
                i = j;
                break;
            }
        }
    }
    return bt;
}

Matcher::result_t Matcher::parseAlignmentRecord(char *data, bool readCompressed) {
    char * entry[255];
    size_t columns = Util::getWordsOfLine(data, entry, 255 );
    char key[255];
    ptrdiff_t keySize =  (entry[1] - data);
    strncpy(key, data, keySize);
    key[keySize] = '\0';

    unsigned int targetId = Util::fast_atoi<unsigned int>(key);
    int score = Util::fast_atoi<int>(entry[1]);
    double seqId = strtod(entry[2],NULL);
    double eval = strtod(entry[3],NULL);

    int qStart =  Util::fast_atoi<int>(entry[4]);
    int qEnd = Util::fast_atoi<int>(entry[5]);
    int qLen = Util::fast_atoi<int>(entry[6]);
    int dbStart = Util::fast_atoi<int>(entry[7]);
    int dbEnd = Util::fast_atoi<int>(entry[8]);
    int dbLen = Util::fast_atoi<int>(entry[9]);
    int adjustQstart = (qStart==-1)? 0 : qStart;
    int adjustDBstart = (dbStart==-1)? 0 : dbStart;
    double qCov = SmithWaterman::computeCov(adjustQstart, qEnd, qLen);
    double dbCov = SmithWaterman::computeCov(adjustDBstart, dbEnd, dbLen);
    size_t alnLength = Matcher::computeAlnLength(adjustQstart, qEnd, adjustDBstart, dbEnd);

    if(columns < ALN_RES_WITH_BT_COL_CNT){
        return Matcher::result_t(targetId, score, qCov, dbCov, seqId, eval,
                                 alnLength, qStart, qEnd, qLen, dbStart, dbEnd,
                                 dbLen, "");
    }else{
        size_t len = entry[11] - entry[10];
        if(readCompressed){
            return Matcher::result_t(targetId, score, qCov, dbCov, seqId, eval,
                                     alnLength, qStart, qEnd, qLen, dbStart, dbEnd,
                                     dbLen, std::string(entry[10], len));
        }else {
            return Matcher::result_t(targetId, score, qCov, dbCov, seqId, eval,
                                     alnLength, qStart, qEnd, qLen, dbStart, dbEnd,
                                     dbLen, uncompressAlignment(std::string(entry[10], len)));
        }
    }
}


size_t Matcher::resultToBuffer(char * buff1, const result_t &result, bool addBacktrace, bool compress) {
    char * basePos = buff1;
    char * tmpBuff = Itoa::u32toa_sse2((uint32_t) result.dbKey, buff1);
    *(tmpBuff-1) = '\t';
    tmpBuff = Itoa::i32toa_sse2(result.score, tmpBuff);
    *(tmpBuff-1) = '\t';
    float seqIdFlt = result.seqId;
    //TODO seqid, evalue
    if(seqIdFlt==1.0){
        *(tmpBuff) = '1';
        tmpBuff++;
        *(tmpBuff) = '.';
        tmpBuff++;
        *(tmpBuff) = '0';
        tmpBuff++;
        *(tmpBuff) = '0';
        tmpBuff++;
        *(tmpBuff) = '0';
        tmpBuff++;
        *(tmpBuff) = '\t';
        tmpBuff++;
    }else{
        *(tmpBuff) = '0';
        tmpBuff++;
        *(tmpBuff) = '.';
        tmpBuff++;
        if(seqIdFlt<0.10){
            *(tmpBuff) = '0';
            tmpBuff++;
        }
        if(seqIdFlt<0.01){
            *(tmpBuff) = '0';
            tmpBuff++;
        }
        int seqId = seqIdFlt*1000;
        tmpBuff = Itoa::i32toa_sse2(seqId, tmpBuff);
        *(tmpBuff-1) = '\t';
    }

    tmpBuff += sprintf(tmpBuff,"%.3E",result.eval);
    tmpBuff++;
    *(tmpBuff-1) = '\t';
    tmpBuff = Itoa::i32toa_sse2(result.qStartPos, tmpBuff);
    *(tmpBuff-1) = '\t';
    tmpBuff = Itoa::i32toa_sse2(result.qEndPos, tmpBuff);
    *(tmpBuff-1) = '\t';
    tmpBuff = Itoa::i32toa_sse2(result.qLen, tmpBuff);
    *(tmpBuff-1) = '\t';
    tmpBuff = Itoa::i32toa_sse2(result.dbStartPos, tmpBuff);
    *(tmpBuff-1) = '\t';
    tmpBuff = Itoa::i32toa_sse2(result.dbEndPos, tmpBuff);
    if(addBacktrace == true){
        *(tmpBuff-1) = '\t';
        tmpBuff = Itoa::i32toa_sse2(result.dbLen, tmpBuff);
        if(compress){
            *(tmpBuff-1) = '\t';
            std::string compressedCigar = Matcher::compressAlignment(result.backtrace);
            tmpBuff = strncpy(tmpBuff, compressedCigar.c_str(), compressedCigar.length());
            tmpBuff+= compressedCigar.length()+1;
        }else{
            *(tmpBuff-1) = '\t';
            tmpBuff = strncpy(tmpBuff, result.backtrace.c_str(), result.backtrace.length());
            tmpBuff+= result.backtrace.length()+1;
        }
    }else{
        *(tmpBuff-1) = '\t';
        tmpBuff = Itoa::i32toa_sse2(result.dbLen, tmpBuff);
    }

    *(tmpBuff-1) = '\n';
    *(tmpBuff) = '\0';
    return tmpBuff - basePos;
}


