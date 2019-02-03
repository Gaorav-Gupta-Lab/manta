//
// Manta - Structural Variant and Indel Caller
// Copyright (c) 2013-2018 Illumina, Inc.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//

/// \file
/// \author Chris Saunders
/// \author Naoki Nariai
///

#include "SVWriter.hh"
#if 0
#include "manta/SVMultiJunctionCandidateUtil.hh"
#include "svgraph/EdgeInfoUtil.hh"

#include "blt_util/log.hh"

#include <iostream>


//#define DEBUG_GSV
#endif



SVWriter::
SVWriter(
    const GSCOptions& initOpt,
    const bam_header_info& bamHeaderInfo,
    const char* progName,
    const char* progVersion,
    const std::vector<std::string>& sampleNames) :
    opt(initOpt),
    diploidSampleCount(opt.alignFileOpt.diploidSampleCount()),
    candfs(opt.candidateOutputFilename),
    dipfs(opt.diploidOutputFilename),
    somfs(opt.somaticOutputFilename),
    tumfs(opt.tumorOutputFilename),
    rnafs(opt.rnaOutputFilename),
    candWriter(opt.referenceFilename, bamHeaderInfo, candfs.getStream(),
               opt.isOutputContig),
    diploidWriter(opt.diploidOpt, (! opt.chromDepthFilename.empty()),
                  opt.referenceFilename, bamHeaderInfo, dipfs.getStream(),
                  opt.isOutputContig),
    somWriter(opt.somaticOpt, (! opt.chromDepthFilename.empty()),
              opt.referenceFilename, bamHeaderInfo,somfs.getStream(),
              opt.isOutputContig),
    tumorWriter(opt.tumorOpt, (! opt.chromDepthFilename.empty()),
                opt.referenceFilename, bamHeaderInfo, tumfs.getStream(),
                opt.isOutputContig),
    rnaWriter(opt.referenceFilename, bamHeaderInfo, rnafs.getStream(),
              opt.isOutputContig)
{
    // Use 'noSampleNames' to force default sample names for the candidate header (why?)
    std::vector<std::string> noSampleNames;
    candWriter.writeHeader(progName, progVersion,noSampleNames);

    if (opt.isTumorOnly())
    {
        tumorWriter.writeHeader(progName, progVersion,sampleNames);
    }
    else if (opt.isRNA)
    {
        rnaWriter.writeHeader(progName, progVersion, sampleNames);
    }
    else
    {
        std::vector<std::string> diploidSampleNames(sampleNames.begin(),sampleNames.begin()+diploidSampleCount);
        diploidWriter.writeHeader(progName, progVersion,diploidSampleNames);
        if (opt.isSomatic()) somWriter.writeHeader(progName, progVersion,sampleNames);
    }
}



static
bool
isAnyFalse(
    const std::vector<bool>& vb)
{
    for (const bool val : vb)
    {
        if (! val) return true;
    }
    return false;
}



void
SVWriter::
writeSV(
    const EdgeInfo& edge,
    const SVCandidateSetData& svData,
    const std::vector<SVCandidateAssemblyData>& mjAssemblyData,
    const SVMultiJunctionCandidate& mjSV,
    const std::vector<bool>& isCandidateJunctionFiltered,
    const std::vector<bool>& isScoredJunctionFiltered,
    const std::vector<SVId>& junctionSVId,
    const std::vector<SVModelScoreInfo>& mjModelScoreInfo,
    const SVModelScoreInfo& mjJointModelScoreInfo,
    const bool isMJEvent,
    SupportSamples& svSupports) const
{
    // write out candidates for each junction independently:
    const unsigned junctionCount(mjSV.junction.size());
    for (unsigned junctionIndex(0); junctionIndex<junctionCount; ++junctionIndex)
    {
        if (isCandidateJunctionFiltered[junctionIndex]) continue;

        const SVCandidateAssemblyData& assemblyData(mjAssemblyData[junctionIndex]);
        const SVCandidate& sv(mjSV.junction[junctionIndex]);
        const SVId& svId(junctionSVId[junctionIndex]);

        candWriter.writeSV(svData, assemblyData, sv, svId);
    }

    if (opt.isSkipScoring) return;

    // check to see if all scored junctions are filtered before writing scored vcf output:
    if (! isAnyFalse(isScoredJunctionFiltered)) return;

    const unsigned unfilteredJunctionCount(std::count(isScoredJunctionFiltered.begin(), isScoredJunctionFiltered.end(), true));

    // setup all event-level info that we need to share across all event junctions:
    bool isMJDiploidEvent(isMJEvent);
    EventInfo event;
    event.junctionCount=unfilteredJunctionCount;

    bool isMJEventWriteDiploid(false);
    bool isMJEventWriteSomatic(false);

    std::vector<bool> isJunctionSampleCheckFail(diploidSampleCount, false);

    if (isMJEvent)
    {
        // sample specific check for the assumption of multi-junction candidates: every junction shares the same genotype.
        // check if the individual junctions should be potentially assigned different genotypes.
        for (unsigned sampleIndex(0); sampleIndex<diploidSampleCount; ++sampleIndex)
        {
            const SVScoreInfoDiploid& jointDiploidInfo(mjJointModelScoreInfo.diploid);
            const SVScoreInfoDiploidSample& jointDiploidSampleInfo(jointDiploidInfo.samples[sampleIndex]);
            const DIPLOID_GT::index_t jointGT(jointDiploidSampleInfo.gt);
            const double jointGtPProb(jointDiploidSampleInfo.pprob[jointGT]);

            if (jointGT == DIPLOID_GT::REF)
            {
                isJunctionSampleCheckFail[sampleIndex] = true;
#ifdef DEBUG_GSV
                if (isJunctionSampleCheckFail[sampleIndex])
                {
                    log_os << __FUNCTION__ << ": The multi-junction candidate has hom-ref for sample #"
                           << sampleIndex << ".\n";
                }
#endif
                continue;
            }

            for (unsigned junctionIndex(0); junctionIndex < junctionCount; ++junctionIndex)
            {
                if (isScoredJunctionFiltered[junctionIndex]) continue;

                const SVScoreInfoDiploid& diploidInfo(mjModelScoreInfo[junctionIndex].diploid);
                const SVScoreInfoDiploidSample& diploidSampleInfo(diploidInfo.samples[sampleIndex]);
                const DIPLOID_GT::index_t singleGT(diploidSampleInfo.gt);
                const double singleGtPProb(diploidSampleInfo.pprob[singleGT]);
                const double deltaGtPProb(jointGtPProb - diploidSampleInfo.pprob[jointGT]);

                // fail the genotype check if
                // 1) the joint event changes the genotype of the junction and increases the posterior prob by more than 0.9
                // 2) the posterior prob is larger than 0.9 when the junction is genotyped individually
                if ((! (jointGT==singleGT)) && (deltaGtPProb > 0.9) && (singleGtPProb > 0.9))
                {
                    isJunctionSampleCheckFail[sampleIndex] = true;
                    break;
                }
            }

#ifdef DEBUG_GSV
            if (isJunctionSampleCheckFail[sampleIndex])
            {
                log_os << __FUNCTION__ << ": The multi-junction candidate failed the check of sample-specific genotyping for sample #"
                       << sampleIndex << ".\n";
            }
#endif
        }

        // if any sample passing the genotype check, the multi-junction diploid event remains valid;
        // otherwise, fail the multi-junction candidate
        if (! isAnyFalse(isJunctionSampleCheckFail)) isMJDiploidEvent = false;

#ifdef DEBUG_GSV
        if (! isMJDiploidEvent)
        {
            log_os << __FUNCTION__ << ": Rejecting the multi-junction candidate, failed the sample-specific check of genotyping for all diploid samples.\n";
        }
#endif

        for (unsigned junctionIndex(0); junctionIndex<junctionCount; ++junctionIndex)
        {
            if (isScoredJunctionFiltered[junctionIndex]) continue;

            // set the Id of the first junction in the group as the event (i.e. multi-junction) label
            if (event.label.empty())
            {
                const SVId& svId(junctionSVId[junctionIndex]);
                event.label = svId.localId;
            }

            const SVModelScoreInfo& modelScoreInfo(mjModelScoreInfo[junctionIndex]);

            // for diploid case only,
            // we decide to use multi-junction or single junction based on best score:
            // (for somatic case a lower somatic score could be due to reference evidence in an event member)
            //
            if (mjJointModelScoreInfo.diploid.filters.size() > modelScoreInfo.diploid.filters.size())
            {
                isMJDiploidEvent=false;
            }
            else if (mjJointModelScoreInfo.diploid.altScore < modelScoreInfo.diploid.altScore)
            {
                isMJDiploidEvent=false;
            }

            // for somatic case,
            // report multi-junction if either the multi-junction OR ANY single junction has a good score.
            if ((mjJointModelScoreInfo.somatic.somaticScore >= opt.somaticOpt.minOutputSomaticScore) ||
                (modelScoreInfo.somatic.somaticScore >= opt.somaticOpt.minOutputSomaticScore))
            {
                isMJEventWriteSomatic = true;
            }

            //TODO: set up criteria for isMJEventWriteTumor
        }

        // for events, we write all junctions, or no junctions,
        // so we need to determine write status over the whole set rather than a single junction
        if (isMJDiploidEvent)
        {
            isMJEventWriteDiploid = (mjJointModelScoreInfo.diploid.altScore >= opt.diploidOpt.minOutputAltScore);
        }
    }

    // final scored output is treated (mostly) independently for each junction:
    //
    for (unsigned junctionIndex(0); junctionIndex<junctionCount; ++junctionIndex)
    {
        if (isScoredJunctionFiltered[junctionIndex]) continue;

        const SVCandidateAssemblyData& assemblyData(mjAssemblyData[junctionIndex]);
        const SVCandidate& sv(mjSV.junction[junctionIndex]);
        const SVModelScoreInfo& modelScoreInfo(mjModelScoreInfo[junctionIndex]);

        const SVId& svId(junctionSVId[junctionIndex]);
        const SVScoreInfo& baseScoringInfo(modelScoreInfo.base);
        static const EventInfo nonEvent;

        if (opt.isTumorOnly())
        {
            //TODO: add logic for MJEvent

            const SVScoreInfoTumor& tumorInfo(modelScoreInfo.tumor);
            tumorWriter.writeSV(svData, assemblyData, sv, svId, baseScoringInfo, tumorInfo, nonEvent);
        }
        else if (opt.isRNA)
        {
            const SVScoreInfoRna& rnaInfo(modelScoreInfo.rna);
            rnaWriter.writeSV(svData, assemblyData, sv, svId, baseScoringInfo, rnaInfo, nonEvent);
        }
        else
        {
            {
                const EventInfo& diploidEvent( isMJDiploidEvent ? event : nonEvent );
                const SVModelScoreInfo& scoreInfo( isMJDiploidEvent ? mjJointModelScoreInfo : modelScoreInfo);
                SVScoreInfoDiploid diploidInfo(scoreInfo.diploid);

                if (isMJDiploidEvent)
                {
                    // if one sample failed the genotype check,
                    // use the sample-specific diploid scoreInfo, instead of the joint scoreInfo
                    for (unsigned sampleIndex(0); sampleIndex<diploidSampleCount; ++sampleIndex)
                    {
                        if (isJunctionSampleCheckFail[sampleIndex])
                        {
#ifdef DEBUG_GSV
                            log_os << __FUNCTION__ << ": Junction #" << junctionIndex
                                   << ": Swapped diploid info for sample #" << sampleIndex << ".\n"
                                   << "Before:" << diploidInfo.samples[sampleIndex] << "\n"
                                   << "After:" << modelScoreInfo.diploid.samples[sampleIndex] << "\n" ;
#endif
                            diploidInfo.samples[sampleIndex] = modelScoreInfo.diploid.samples[sampleIndex];
                        }
                    }
                }

                bool isWriteDiploid(false);
                if (isMJDiploidEvent)
                {
                    isWriteDiploid = isMJEventWriteDiploid;
                }
                else
                {
                    isWriteDiploid = (modelScoreInfo.diploid.altScore >= opt.diploidOpt.minOutputAltScore);
                }

                if (isWriteDiploid)
                {
                    diploidWriter.writeSV(svData, assemblyData, sv, svId, baseScoringInfo, diploidInfo, diploidEvent, modelScoreInfo.diploid);
                }
            }

            if (opt.isSomatic())
            {
                const EventInfo& somaticEvent( isMJEvent ? event : nonEvent );
                const SVModelScoreInfo& scoreInfo(isMJEvent ? mjJointModelScoreInfo : modelScoreInfo);
                const SVScoreInfoSomatic& somaticInfo(scoreInfo.somatic);

                bool isWriteSomatic(false);

                if (isMJEvent)
                {
                    isWriteSomatic = isMJEventWriteSomatic;
                }
                else
                {
                    isWriteSomatic = (modelScoreInfo.somatic.somaticScore >= opt.somaticOpt.minOutputSomaticScore);
                }

                if (isWriteSomatic)
                {
                    somWriter.writeSV(svData, assemblyData, sv, svId, baseScoringInfo, somaticInfo, somaticEvent, modelScoreInfo.somatic);
                }
            }
        }
    }
}