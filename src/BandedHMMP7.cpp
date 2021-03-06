/*******************************************************************************
 * This file is part of HmmUFOtu, an HMM and Phylogenetic placement
 * based tool for Ultra-fast taxonomy assignment and OTU organization
 * of microbiome sequencing data with species level accuracy.
 * Copyright (C) 2017  Qi Zheng
 *
 * HmmUFOtu is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HmmUFOtu is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with AlignerBoost.  If not, see <http://www.gnu.org/licenses/>.
 *******************************************************************************/
/*
 * BandedHMMP7.cpp
 *
 *  Created on: May 13, 2015
 *      Author: zhengqi
 */

#include <math.h> /* using C99 */
#include <cstdlib>
#include <cassert>
#include <limits>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <stdexcept>
#include <ctime>
#include <sstream>
#include <algorithm>
#include "BandedHMMP7.h"
#include "HmmUFOtuConst.h"
#include "LinearAlgebraBasic.h"

namespace EGriceLab {
namespace HmmUFOtu {

using namespace std;
using namespace Eigen;

/*const int BandedHMMP7::kMinProfile = 10000; // up-to 10K 16S rRNA profile*/
const string BandedHMMP7::HMM_TAG =
		"HMM\t\tA\tC\tG\tT\n\t\tm->m\tm->i\tm->d\ti->m\ti->i\td->m\td->d";
const string BandedHMMP7::HmmAlignment::TSV_HEADER = "seq_start\tseq_end\thmm_start\thmm_end\tCS_start\tCS_end\tcost\talignment";

const double BandedHMMP7::kMinGapFrac = 0.2;
const double BandedHMMP7::CONS_THRESHOLD = 0.9;
const double BandedHMMP7::DEFAULT_ERE = 1;
const IOFormat tabFmt(StreamPrecision, DontAlignCols, "\t", "\n", "", "", "", "");

BandedHMMP7::BandedHMMP7() :
		name("unnamed"), K(0), L(0), abc(NULL),
		hmmBg(0), nSeq(0), effN(0), wingRetracted(false) {
	/* Assert IEE559 at construction time */
	assert(std::numeric_limits<double>::is_iec559);
}

BandedHMMP7::BandedHMMP7(const string& name, int K, const DegenAlphabet* abc) :
		name(name), K(K), L(0), abc(abc),
		hmmBg(K), nSeq(0), effN(0),
		cs2ProfileIdx() /* zero initiation */, profile2CSIdx() /* zero initiation */,
		wingRetracted(false) {
	if(!(abc->getAlias() == "DNA" && abc->getSize() == 4))
		throw invalid_argument("BandedHMMP7 only supports DNA alphabet");
	/* Assert IEE559 at construction time */
	assert(numeric_limits<double>::is_iec559);
	init_transition_params();
	init_emission_params();
	init_special_params();
	init_limits();
	enableProfileLocalMode(); // always in profile local alignment mode
	setSpEmissionFreq(); // set special emissions by default method
}

BandedHMMP7::BandedHMMP7(const string& name, const string& hmmVersion, int K, const DegenAlphabet* abc) :
		name(name), hmmVersion(hmmVersion), K(K), L(0), abc(abc),
		hmmBg(K), nSeq(0), effN(0),
		cs2ProfileIdx() /* zero initiation */, profile2CSIdx() /* zero initiation */,
		wingRetracted(false) {
	if(!(abc->getAlias() == "DNA" && abc->getSize() == 4))
		throw invalid_argument("BandedHMMP7 only supports DNA alphabet");
	/* Assert IEE559 at construction time */
	assert(numeric_limits<double>::is_iec559);
	init_transition_params();
	init_emission_params();
	init_special_params();
	init_limits();
	enableProfileLocalMode(); // always in profile local alignment mode
	setSpEmissionFreq(); // set special emissions by default method
}

/* non-member friend functions */
istream& operator>>(istream& in, BandedHMMP7& hmm) {
	string line;
	int k = 0; // pos on the profile
	while (getline(in, line)) {
		if (line == "//") {/* end of profile */
			hmm.extend_index();
			hmm.resetProbByCost(); // set the cost matrices
			hmm.adjustProfileLocalMode();
			hmm.wingRetract();
			return in;
		}
		istringstream iss(line); // detail parse this line
		string tag; /* header tag names and values */
		string tmp;
		if (!isspace(line[0])) { /* header section starts with non-empty characters */
			iss >> tag;
			if (tag.substr(0, 6) == "HMMER3") { // do not override our version, check minor version
				if(tag.length() < 8 || tag[7] < 'f') {
					cerr << "Obsolete HMM file version: " << tag << ", must be HMMER3/f or higher" << endl;
					in.setstate(ios_base::badbit);
					return in;
				}
			}
			else if (tag == "NAME") {
				iss >> hmm.name;
			} else if (tag == "LENG") {
				iss >> hmm.K;
				hmm.setProfileSize();
				hmm.enableProfileLocalMode(); // always in profile local alignment mode
				hmm.setSpEmissionFreq(); // set special emissions by default method
			} else if (tag == "ALPH") {
				string abc;
				iss >> abc;
				if (abc != "DNA")
					throw invalid_argument(
							"Not allowed alphabet '" + abc
									+ "' in the HMM input file! Must be DNA");
				// override the alphabet
				hmm.abc = AlphabetFactory::getAlphabetByName("DNA");
			} else if(tag == "MAXL") {
				iss >> hmm.L;
			} else if (tag == "STATS") {
				string mode;
				string distrib;
				iss >> mode >> distrib;
				tag += " " + mode + " " + distrib; // use STATS + mode + distribution as the new tag name
				string val;
				getline(iss, val);
				hmm.setOptTag(tag, BandedHMMP7::trim(val));

			} else if(tag == "HMM") { /* HMM TAG */
				string tmp;
				getline(in, tmp); /* ignore the next line too */
			}
			else { /* optional tags */
				string val;
				getline(iss, val); // get the entire remaining part of this line
				if(!tag.empty())
					hmm.setOptTag(tag, BandedHMMP7::trim(val)); // record this tag-value pair
				// check some optional tags
				if(tag == "NSEQ")
					hmm.nSeq = ::atoi(val.c_str());
				else if(tag == "EFFN")
					hmm.effN = ::atof(val.c_str());
				else
				{ /* do nothing */ }
			}
		} /* end of header section */
		else { /* Main body, starts with space */
			iss >> tag;
			if (tag == "COMPO" || BandedHMMP7::isInteger(tag)) { // A compo line can be treated as position 0
				assert((tag == "COMPO" && k == 0) || atoi(tag.c_str()) == k);
				/* process current emission line */
				Vector4d emitFreq;
				for (Vector4d::Index i = 0; i < 4; ++i)
					iss >> emitFreq(i);
				if (tag == "COMPO") { // COMPO line
					hmm.E_M_cost.col(0) = emitFreq;
					emitFreq = (-emitFreq).array().exp();
					hmm.setSpEmissionFreq(emitFreq);
					hmm.hmmBg.setBgFreq(emitFreq);
				} else {
					/* Mk emission line */
					hmm.E_M_cost.col(k) = emitFreq;
					/* Make sure the MAP tag is set */
					string val;
					if(hmm.getOptTag("MAP") != "yes") {
						cerr << "Error: HMM file must has the MAP flag set to 'yes'" << endl;
						in.setstate(ios_base::badbit);
						return in;
					}
					iss >> tmp;
					hmm.cs2ProfileIdx[atoi(tmp.c_str())] = k;
					hmm.profile2CSIdx[k] = atoi(tmp.c_str());
					hmm.setLocOptTag("MAP", tmp, k);
					/* read other optional tags */
					if(!hmm.getOptTag("CONS").empty()) { /* this tag is present, regarding yes or no */
						iss >> tmp;
						hmm.setLocOptTag("CONS", tmp, k);
					}
					if(!hmm.getOptTag("RF").empty()) { /* this tag is present, regarding yes or no */
						iss >> tmp;
						hmm.setLocOptTag("RF", tmp, k);
					}
					if(!hmm.getOptTag("MM").empty()) { /* this tag is present, regarding yes or no */
						iss >> tmp;
						hmm.setLocOptTag("MM", tmp, k);
					}
					if(!hmm.getOptTag("CS").empty()) { /* this tag is present, regarding yes or no */
						iss >> tmp;
						hmm.setLocOptTag("CS", tmp, k);
					}
				}
				/* process the following Ik emission line */
				for (MatrixXd::Index i = 0; i < hmm.E_I_cost.rows(); ++i)
					in >> hmm.E_I_cost(i, k);
				/* process the following state K transition line */
					in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::M, BandedHMMP7::M) = hmm.hmmValueOf(tmp);  // Mk -> Mk+1
					in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::M, BandedHMMP7::I) = hmm.hmmValueOf(tmp);  // Mk -> Ik
					in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::M, BandedHMMP7::D) = hmm.hmmValueOf(tmp);  // Mk -> Dk+1
					in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::I, BandedHMMP7::M) = hmm.hmmValueOf(tmp);  // Ik -> Mk+1
					in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::I, BandedHMMP7::I) = hmm.hmmValueOf(tmp);  // Ik -> Ik
					in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::D, BandedHMMP7::M) = hmm.hmmValueOf(tmp);  // Dk -> Mk+1
					in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::D, BandedHMMP7::D) = hmm.hmmValueOf(tmp);  // Dk -> Dk+1
			} /* combo line section or match state line section */
			else { // non-COMPO begin state line (M0)
				assert(k == 0);
				string tmp;
				/* process the BEGIN insert emission line */
				for (MatrixXd::Index i = 0; i < hmm.E_I_cost.rows(); ++i)
					in >> hmm.E_I_cost(i, k);
				/* process the B state K transition line */
				in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::M, BandedHMMP7::M) = hmm.hmmValueOf(tmp);  // Mk -> Mk+1
				in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::M, BandedHMMP7::I) = hmm.hmmValueOf(tmp);  // Mk -> Ik
				in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::M, BandedHMMP7::D) = hmm.hmmValueOf(tmp);  // Mk -> Dk+1
				in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::I, BandedHMMP7::M) = hmm.hmmValueOf(tmp);  // Ik -> Mk+1
				in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::I, BandedHMMP7::I) = hmm.hmmValueOf(tmp);  // Ik -> Ik
				in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::D, BandedHMMP7::M) = hmm.hmmValueOf(tmp);  // Dk -> Mk+1
				in >> tmp; hmm.Tmat_cost[k](BandedHMMP7::D, BandedHMMP7::D) = hmm.hmmValueOf(tmp);  // Dk -> Dk+1
			}
			k++;
		} /* end of main section */
	} /* end of each line */
	// somehow the hmm file reached end without '//'
	in.setstate(std::ios::failbit);
	return in;
}

void BandedHMMP7::scale(double r) {
	/* scale transitions */
	for(int k = 0; k <= K; ++k)
		Tmat[k] *= r;
	/* scale emissions */
	E_M *= r;
	E_I *= r;
	/* reset costs */
	resetCostByProb();
}

void BandedHMMP7::normalize() {
	for(int k = 0; k <= K; ++k) {
		/* normalize transitions */
		Tmat[k].row(M) /= Tmat[k].row(M).sum(); /* TMX */
		Tmat[k].row(I) /= Tmat[k].row(I).sum(); /* TIX */
		Tmat[k].row(D) /= Tmat[k].row(D).sum(); /* TDX */
		/* normalize emissions */
		E_M.col(k) /= E_M.col(k).sum(); /* EM */
		E_I.col(k) /= E_I.col(k).sum(); /* EI */
	}
	/* enforce the T[0] and T[K] specials */
	Tmat[0](D, M) = 1;
	Tmat[0](D, D) = 0;
	Tmat[K](M, D) = 0;
	Tmat[K](D, M) = 1;
	Tmat[K](D, D) = 0;

	/* reset costs */
	resetCostByProb();
}

void BandedHMMP7::estimateParams(const BandedHMMP7Prior& prior) {
	assert(abc->getSize() == prior.dmME.getK());

	/* normalize the COMPO Match emission, which is the B state emission */
//	E_M.col(0) /= E_M.col(0).sum();

	/* re-estimate parameters using the prior info */
	for(int k = 0; k <= K; ++k) {
		/* update transition parameters */
		/* TM */
		Tmat[k].row(M) = prior.dmMT.meanPostP(Tmat[k].row(M));
		/* TI */
		Tmat[k].row(I).segment(M, 2) = prior.dmIT.meanPostP(Tmat[k].row(I).segment(M, 2)); /* only use first two elements of the TI row */
		/* TD */
		VectorXd dt(2);
		dt(0) = Tmat[k](D, M);
		dt(1) = Tmat[k](D, D);
		dt = prior.dmDT.meanPostP(dt); /* replace observed frequency with meanPostP */
		Tmat[k](D, M) = dt(0);
		Tmat[k](D, D) = dt(1);

		/* update emission parameters */
		E_M.col(k) = prior.dmME.meanPostP(E_M.col(k));
		E_I.col(k) = prior.dmIE.meanPostP(E_I.col(k));
	}

	/* enforce the T[0] and T[K] specials */
	Tmat[0](D, M) = 1;
	Tmat[0](D, D) = 0;
	Tmat[K](M, D) = 0;
	Tmat[K](D, M) = 1;
	Tmat[K](D, D) = 0;

	/* reset costs */
	resetCostByProb();
}

double BandedHMMP7::meanRelativeEntropy() const {
	double ent = 0;
	for(int k = 1; k <= K; ++k)
		ent += Math::relative_entropy(E_M.col(k), hmmBg.getBgEmitPr());
	return ent / K;
}

ostream& operator<<(ostream& out, const BandedHMMP7& hmm) {
	/* write mandatory tags */
	out << "HMMER3/f\t" << hmm.hmmVersion << endl;
	out << "NAME\t" << hmm.name << endl;
	out << "LENG\t" << hmm.K << endl;
	out << "ALPH\t" << hmm.abc->getAlias() << endl;

	/* write optional tags */
	for(vector<string>::const_iterator it = hmm.optTagNames.begin(); it != hmm.optTagNames.end(); ++it)
		out << *it << "  " << hmm.getOptTag(*it) << endl;

	/* write optional HMM tags */
	out << BandedHMMP7::HMM_TAG << endl;
	for(int k = 0; k <= hmm.K; ++k) {
		/* write M or background emission line */
		if(k == 0)
			out << "\tCOMPO\t" << hmm.E_M_cost.col(0).transpose().format(tabFmt) << endl;
		else {
			out << "\t" << k << "\t" << hmm.E_M_cost.col(k).transpose().format(tabFmt);
			/* write other optional tags, if present */
			if(!hmm.getOptTag("MAP").empty())
				out << "\t" << hmm.getLocOptTag("MAP", k);
			if(!hmm.getOptTag("CONS").empty())
				out << "\t" << hmm.getLocOptTag("CONS", k);
			if(!hmm.getOptTag("RF").empty())
				out << "\t" << hmm.getLocOptTag("RF", k);
			if(!hmm.getOptTag("MM").empty())
				out << "\t" << hmm.getLocOptTag("MM", k);
			if(!hmm.getOptTag("CS").empty())
				out << "\t" << hmm.getLocOptTag("CS", k);
			out << endl;
		}
		/* write insert emission line */
		double val;
		out << "\t";
		for(MatrixXd::Index i = 0; i != hmm.E_I_cost.rows(); ++i) {
			val = hmm.E_I_cost(i, k);
			hmmPrintValue(out << "\t", val);
		}
		out << endl;

		/* write state transition line */
		val = hmm.Tmat_cost[k](BandedHMMP7::M, BandedHMMP7::M); hmmPrintValue(out << "\t\t", val);
		val = hmm.Tmat_cost[k](BandedHMMP7::M, BandedHMMP7::I); hmmPrintValue(out << "\t", val);
		val = hmm.Tmat_cost[k](BandedHMMP7::M, BandedHMMP7::D); hmmPrintValue(out << "\t", val);
		val = hmm.Tmat_cost[k](BandedHMMP7::I, BandedHMMP7::M); hmmPrintValue(out << "\t", val);
		val = hmm.Tmat_cost[k](BandedHMMP7::I, BandedHMMP7::I); hmmPrintValue(out << "\t", val);
		val = hmm.Tmat_cost[k](BandedHMMP7::D, BandedHMMP7::M); hmmPrintValue(out << "\t", val);
		val = hmm.Tmat_cost[k](BandedHMMP7::D, BandedHMMP7::D); hmmPrintValue(out << "\t", val);

		out << endl;
	}
	out << "//" << endl;
	return out;
}

ostream& operator<<(ostream& os, const deque<BandedHMMP7::p7_state> path) {
	for(deque<BandedHMMP7::p7_state>::const_iterator it = path.begin(); it != path.end(); ++it)
		os << BandedHMMP7::decode(*it);
	return os;
}

BandedHMMP7& BandedHMMP7::build(const MSA& msa, double symfrac,
		const BandedHMMP7Prior& prior, const string& name) {
	if(msa.getMSALen() == 0)
		throw invalid_argument("Empty MSA encountered");
	if(!(symfrac > 0 && symfrac < 1))
		throw invalid_argument("symfrac must between 0 and 1");

	/* set basic info and index */
	if(!name.empty())
		this->name = name;
	else
		this->name = msa.getName();
	abc = msa.getAbc();
	reset_index();
	/* set/determine the bHMM size */
	L = msa.getCSLen();
	const unsigned N = msa.getNumSeq();
	unsigned k = 0;

	for(unsigned j = 0; j < L; ++j) {
		if(msa.symWFrac(j) >= symfrac)
			profile2CSIdx[++k] = j + 1; /* all index are 1-based */
		cs2ProfileIdx[j+1] = k;
	}
	/* profile size calculated as current k */
	setProfileSize(k);

	/* set CONS values */
	char csLoc[32];
	for(int k = 1; k <= K; ++k) {
		sprintf(csLoc, "%d", profile2CSIdx[k]);
		setLocOptTag("CONS", csLoc, k);
	}

	/* reset/init transition and emisison matrices */
	reset_transition_params();
	reset_emission_params();

	/* train the hmm model using observed count, all index are 1-based */
	for(unsigned j = 1; j <= L; ++j) {
		k = cs2ProfileIdx[j];
		for(unsigned i = 1; i <= N; ++i) {
			int8_t b = msa.encodeAt(i - 1, j - 1);
			double w = msa.getSeqWeight(i - 1); /* use weighted count */
			p7_state sm = determineMatchingState(cs2ProfileIdx, j, b);
			if(sm == P)
				continue; // ignore this base
//			cerr << "j:" << j << " k:" << k << " i:" << i << " sm:" << sm << endl;
			/* update emission frequencies */
			if(sm == M) {
//				cerr << "i:" << i << " j:" << j << " b:" << (int) b << " db:" << hmm.abc->decode(b) << " k:" << k << endl;
				E_M(b, 0) += w; /* M0 as the COMPO freq */
				E_M(b, k) += w;
			}
			else if(sm == I) {
//				cerr << "i:" << i << " j:" << j << " b:" << (int) b << " db:" << hmm.abc->decode(b) << " k:" << k << endl;
				E_I(b, k) += w;
			}
			else { } // no emission

			/* update transition frequencies */
			unsigned jN;
			p7_state smN;
			/* find the next non P loc on this seq */
			for(jN = j + 1; jN <= L; ++jN) {
				int8_t bN = msa.encodeAt(i - 1, jN - 1);
				p7_state smN = determineMatchingState(cs2ProfileIdx, jN, bN);
				if(smN != P)
					break;
			}
			if(!(jN <= L && smN != P)) // no jN found
				continue;
			unsigned kN = cs2ProfileIdx[jN];
			if(sm == I && smN == D || sm == D && smN == I) // no I->D or D->I allowed
				continue;
//			if(sm == D && (j < msa->seqStart(i) + 1 || j > msa->seqEnd(i) + 1)) // 5' and 3' hanging gaps are ignored
//				continue;
			Tmat[k](sm, smN) += w;
		} // end each seq
	} // end each loc
	/* update B->M1/I0/D1 and MK/IK/DK->E frequencies */
	for(unsigned i = 0; i < N; ++i) {
		double w = msa.getSeqWeight(i);
		int start = msa.seqStart(i);
		int end = msa.seqEnd(i);
		int8_t bStart = msa.encodeAt(i, start);
		p7_state smStart = determineMatchingState(cs2ProfileIdx, start + 1, bStart);
		Tmat[0](M, smStart) += w;
		int8_t bEnd = msa.encodeAt(i, end);
		p7_state smEnd = determineMatchingState(cs2ProfileIdx, end + 1, bEnd);
		Tmat[K](smEnd, M) += w;
	}

	nSeq = msa.getNumSeq();
	effN = nSeq;

	/* tune the effN to target mean relative entropy */
	RelativeEntropyTargetFunc entFunc(DEFAULT_ERE, *this, prior);
	Math::RootFinder rf(entFunc, 0, nSeq);
	effN = rf.rootBisection();
	if(::isnan(effN)) /* failed to estimate effN */
		effN = nSeq;
//	cerr << "Final HMM EFFN: " << hmm.effN << endl;
	scale(effN / nSeq);
	estimateParams(prior);

	/* set bgFreq */
	hmmBg.setBgFreq(E_M.col(0));
	setSpEmissionFreq(E_M.col(0));

	/* set optional tags */
	char value[128];
	sprintf(value, "%d", L);
	setOptTag("MAXL", value);

	setOptTag("RF", "no");

	setOptTag("MM", "no");

	setOptTag("CONS", "yes");

	setOptTag("CS", "no");

	setOptTag("MAP", "yes");

	sprintf(value, "%d", nSeq);
	setOptTag("NSEQ", value);

	sprintf(value, "%g", effN);
	setOptTag("EFFN", value);

	/* set locOptTags */
	locOptTags["CONS"].resize(K + 1);
	locOptTags["MAP"].resize(K + 1);
	for(int k = 1; k <= K; ++k) {
		int map = profile2CSIdx[k];
		sprintf(value, "%d", map);
		setLocOptTag("MAP", value, k);
		char c = msa.CSBaseAt(map);
		int8_t b = abc->encode(c);
		if(msa.wIdentityAt(map) < CONS_THRESHOLD)
			c = ::tolower(c);
		setLocOptTag("CONS", string() + c, k);
	}

	/* set DATE tag after all done */
	time_t rawtime;
	struct tm* timeinfo;
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	strftime(value, 128, "%c", timeinfo);
	setOptTag("DATE", value);

	return *this;
}

string BandedHMMP7::trim(const string& str, const string& whitespace) {
	const string::size_type strBegin = str.find_first_not_of(whitespace);
	if(strBegin == string::npos) // no content
		return "";
	string::size_type strRange = str.find_last_not_of(whitespace) - strBegin + 1;
	return str.substr(strBegin, strRange);
}


void BandedHMMP7::setProfileSize(int size) {
	K = size; // set self size
	hmmBg.setSize(size); // set bg size
	init_transition_params();
	init_emission_params();
	init_special_params();
	init_limits();
}

void BandedHMMP7::setSequenceMode(enum align_mode mode) {
	switch (mode) {
	case GLOBAL:
		T_SP(N, N) = T_SP(C, C) = 0;
		break;
	case LOCAL:
		T_SP(N, N) = T_SP(C, C) = hmmBg.getBgTermPr();
		break;
	case NGCL:
		T_SP(N, N) = 0;
		T_SP(C, C) = hmmBg.getBgTermPr();
		break;
	case CGNL:
		T_SP(N, N) = hmmBg.getBgTermPr();
		T_SP(C, C) = 0;
		break;
	default:
		break; // do nothing
	}
	T_SP(N, B) = 1.0 - T_SP(N, N);
	T_SP(E, C) = 1.0; // always exit from E->C
	T_SP_cost = -T_SP.array().log(); // Eigen3 handle array to matrix assignment automatically
}

void BandedHMMP7::setSpEmissionFreq(const Vector4d& freq) {
	E_SP.col(N) = E_SP.col(C) = freq / freq.sum(); // re-do normalization, even if already done
	E_SP.col(B) = E_SP.col(E) = Vector4d::Zero(); // no emission for state B and E
	E_SP_cost = -E_SP.array().log();
}

void BandedHMMP7::init_transition_params() {
	/* state 0 serves as the B state */
	Tmat.clear();
	Tmat_cost.clear();
	for(int k = 0; k <= K; ++k) {
		Tmat.push_back(Matrix3d::Zero());
		Tmat_cost.push_back(Matrix3d::Constant(inf));
	}
}

void BandedHMMP7::init_emission_params() {
	/* state 0 serves as B state */
	E_M = E_I = Matrix4Xd::Zero(4, K + 1);
	E_M_cost = E_I_cost = Matrix4Xd::Constant(4, K + 1, inf);
}

void BandedHMMP7::init_special_params() {
	/* entry and exit vectors */
	entryPr = exitPr = VectorXd::Zero(K + 1);
	entryPr_cost = exitPr_cost = VectorXd::Constant(K + 1, inf);
	/* special matrices */
	E_SP = Matrix4Xd::Zero(4, kNS);
	E_SP_cost = Matrix4Xd::Constant(4, kNS, inf);
	T_SP = MatrixXd::Zero(kNS, kNS);
	T_SP_cost = MatrixXd::Constant(kNS, kNS, inf);
}

void BandedHMMP7::reset_transition_params() {
	/* state 0 serves as the B state */
	if(!(Tmat.size() != K + 1 && Tmat_cost.size() != K + 1)) /* need initiation instead of reset */
		return init_transition_params();

	for(int k = 0; k <= K; ++k) {
		Tmat[k].setZero();
		Tmat_cost[k].setConstant(inf);
	}
}

void BandedHMMP7::reset_emission_params() {
	/* state 0 serves as B state */
	if(!(E_M.cols() == K + 1 && E_I.cols() == K + 1
			&& E_M_cost.cols() == K + 1 && E_I_cost.cols() == K + 1)) /* need initiation instead of reset */
		return init_emission_params();

	E_M.setZero();
	E_I.setZero();
	E_M_cost.setConstant(inf);
	E_I_cost.setConstant(inf);
}

/*void BandedHMMP7::normalize_transition_params() {

	 * state 0 serves as the B state

	for(int k = 0; k <= K; ++k) {
		for(int i = 0; i < BandedHMMP7::kNM; ++i) {
			double C = Tmat[k].row(i).sum();
			double pseudoC = BandedHMMP7::pseudoCount(C);
			Tmat[k].row(i).array() += pseudoC / Tmat[k].cols();
			Tmat[k].row(i) /= C + pseudoC;
		}
		Tmat_cost[k] = Tmat[k].array().log();
	}

}*/

/*void BandedHMMP7::normalize_emission_params() {
	 state 0 serves as B state
	for(int k = 0; k <= K; ++k) {
		double emC = E_M.col(k).sum();
		double eiC = E_I.col(k).sum();
		if(emC > 0) {
			double emPseudo = BandedHMMP7::pseudoCount(emC);
			E_M.col(k).array() += emPseudo / E_M.rows();
			E_M.col(k) /= emC + emPseudo;
		}
		else {
			E_M.col(k).fill(1.0 / E_M.rows());  Nothing observed, use constants
		}

		if(eiC > 0) {
			double eiPseudo = BandedHMMP7::pseudoCount(eiC);
			E_I.col(k).array() += eiPseudo / E_I.rows();
			E_I.col(k) /= eiC + eiPseudo;
		}
		else {
			E_I.col(k).fill(1.0 / E_I.rows());  Nothing observed, use constants
		}
	}
	E_M_cost = -E_M.array().log();
	E_I_cost = -E_I.array().log();
}*/

void BandedHMMP7::init_limits() {
	gapBeforeLimit = gapAfterLimit = VectorXi(K + 1);
	//delBeforeLimit = delAfterLimit = VectorXi(K + 1);
	for(VectorXi::Index j = 1; j <= K; ++j) {
		gapBeforeLimit(j) = j * kMinGapFrac;
		gapAfterLimit(j) = (K - j) * kMinGapFrac;
	}
}

void BandedHMMP7::reset_index() {
	/* position 0 is dummy for all indices */
	for(int i = 0; i < kMaxProfile; ++i)
		cs2ProfileIdx[i] = 0;
	for(int i = 1; i < kMaxCS; ++i)
		profile2CSIdx[i] = 0;
}

void BandedHMMP7::extend_index() {
	/* extend index upto maxLen */
	for(int i = profile2CSIdx[K] + 1; i <= L && i < kMaxProfile; ++i)
		cs2ProfileIdx[i] = K;
}

void BandedHMMP7::enableProfileLocalMode() {
	/* set entering costs */
	entryPr(0) = 0; // B->B not allowed
	entryPr.segment(1, K).setConstant(1 - hmmBg.getBgTransPr()); /* B->M1..MK equal cost */

	/* set exiting costs */
	exitPr(0) = 0; // B->E not allowed
	exitPr.segment(1, K).setConstant(1 - hmmBg.getBgTransPr()); /* M1..MK ->E equal cost */

	/* set log versions */
	entryPr_cost = -entryPr.array().log();
	exitPr_cost = -exitPr.array().log();
}

void BandedHMMP7::adjustProfileLocalMode() {
	/* adjust entering costs */
	entryPr(0) = 0; // B->B not allowed
	entryPr.segment(1, K).setConstant(Tmat[0](M, M)); /* B->M1..MK equal cost */

	/* set exiting costs */
	exitPr(0) = 0; // B->E not allowed
	exitPr.segment(1, K).setConstant(Tmat[K](M, M)); /* M1..MK ->E equal cost */

	/* set log versions */
	entryPr_cost = -entryPr.array().log();
	exitPr_cost = -exitPr.array().log();
}

BandedHMMP7::ViterbiScores& BandedHMMP7::prepareViterbiScores(ViterbiScores& vs) const {
	vs.DP_M(0, 0) = vs.DP_I(0, 0) = vs.DP_D(0, 0) = inf; /* B->B not possible */
	/* Initialize the M(,0), the B state */
	for (int i = 1; i <= vs.L; i++)
		vs.DP_M(i, 0) = i == 1 ? 0 /* no N->N loop */ : T_SP_cost(N, N) * (i - 1); /* N->N loops */
	vs.DP_M.col(0).array() += T_SP_cost(N, B); /* N->B */

	/* set the I(,0), the B state as M(,0) */
	vs.DP_I.col(0) = vs.DP_M.col(0);

	return vs;
}

void BandedHMMP7::calcViterbiScores(const PrimarySeq& seq, ViterbiScores& vs) const {
	assert(seq.length() == vs.L);
	assert(wingRetracted);

	const int L = vs.L;
	prepareViterbiScores(vs);

	/* Full Dynamic-Programming at row-first order */
	for (int j = 1; j <= K; ++j) {
		for (int i = 1; i <= L; ++i) {
			vs.DP_M(i, j) = E_M_cost(seq.encodeAt(i-1), j) + BandedHMMP7::min(
					static_cast<double> (vs.DP_M(i, 0) + entryPr_cost(j)), // from the B state
					static_cast<double> (vs.DP_M(i - 1, j - 1) + Tmat_cost[j-1](M, M)), // from Mi-1,j-1
					static_cast<double> (vs.DP_I(i - 1, j - 1) + Tmat_cost[j-1](I, M)), // from Ii-1,j-1
					static_cast<double> (vs.DP_D(i - 1, j - 1) + Tmat_cost[j-1](D, M))); // from Di-1,j-1
			vs.DP_I(i, j) = E_I_cost(seq.encodeAt(i - 1), j) + std::min(
							static_cast<double> (vs.DP_M(i - 1, j) + Tmat_cost[j](M, I)), // from Mi-1,j
							static_cast<double> (vs.DP_I(i - 1, j) + Tmat_cost[j](I, I))); // from Ii-1,j
			if(j > 1 && j < K) /* D1 and Dk are retracted */
				vs.DP_D(i, j) = std::min(
						static_cast<double> (vs.DP_M(i, j - 1) + Tmat_cost[j-1](M, D)), // from Mi,j-1
						static_cast<double> (vs.DP_D(i, j - 1) + Tmat_cost[j-1](D, D))); // from Di,j-1
		}
	}
	vs.S.leftCols(K + 1) = vs.DP_M; // 0..K columns copied from the calculated DP_M
	vs.S.col(K + 1) = vs.DP_I.col(K);
	// add M-E exit costs
	vs.S.leftCols(K + 1).rowwise() += exitPr_cost.transpose();
	vs.S.col(K + 1).array() += Tmat_cost[K](I, M); // IK->E
	vs.S.array() += T_SP_cost(E, C); // add E->C transition
	for (int i = 1; i < L; ++i) // S(L,) doesn't have a C-> loop
		vs.S.row(i).array() += T_SP_cost(C, C) * (L - i); // add L-i C->C circles
}

void BandedHMMP7::calcViterbiScores(const PrimarySeq& seq,
		ViterbiScores& vs, const vector<ViterbiAlignPath>& vpaths) const {
	assert(seq.length() == vs.L);
	assert(wingRetracted);

	const int L = vs.L;
	if(vpaths.empty()) // no known path provided, do nothing
		return;

	prepareViterbiScores(vs);

	/* process each known path upstream and themselves */
	for(vector<VPath>::const_iterator vpath = vpaths.begin(); vpath != vpaths.end(); ++vpath) {
		/* Determine banded boundaries */
		int upQLen = vpath == vpaths.begin() /* first path ? */ ? vpath->from - 1 : vpath->from - (vpath - 1)->to;
		if(upQLen < 0)
			upQLen = 0;
		int up_start = vpath == vpaths.begin() /* first path ? */ ? vpath->start - upQLen * (1 + kMinGapFrac) : (vpath - 1)->end;
		if (up_start < 1)
			up_start = 1;
		int up_from = vpath == vpaths.begin() /* first path */ ? vpath->from - upQLen * (1 + kMinGapFrac) : (vpath - 1)->to;
		if (up_from < 1)
			up_from = 1;
//		cerr << "upQLen:" << upQLen << endl;
//		cerr << "up_start:" << up_start << " up_end:" << vpath->start << endl;
//		cerr << "up_from:" << up_from << " up_to:" << vpath->from << endl;

		/* Dynamic programming of upstream of this known path at row-first order */
		for (int j = up_start; j <= vpath->start; ++j) {
			for (int i = up_from; i <= vpath->from; ++i) {
				vs.DP_M(i, j) = E_M_cost(seq.encodeAt(i - 1), j)
						+ BandedHMMP7::min(
								static_cast<double>(vs.DP_M(i, 0) + entryPr_cost(j)), // from B state
								static_cast<double>(vs.DP_M(i - 1, j - 1) + Tmat_cost[j-1](M, M)), // from Mi-1,j-1
								static_cast<double>(vs.DP_I(i - 1, j - 1) + Tmat_cost[j-1](I, M)), // from Ii-1,j-1
								static_cast<double>(vs.DP_D(i - 1, j - 1) + Tmat_cost[j-1](D, M))); // from Di-1,j-1
				vs.DP_I(i, j) = E_I_cost(seq.encodeAt(i - 1), j)
								+ std::min(
										static_cast<double>(vs.DP_M(i - 1, j) + Tmat_cost[j](M, I)), // from Mi-1,j
										static_cast<double>(vs.DP_I(i - 1, j) + Tmat_cost[j](I, I))); // from Ii-1,j
				if(j > 1 && j < K) /* D1 and Dk are retracted */
					vs.DP_D(i, j) =	std::min(
							static_cast<double>(vs.DP_M(i, j - 1) + Tmat_cost[j-1](M, D)), // from Mi,j-1
							static_cast<double>(vs.DP_D(i, j - 1) + Tmat_cost[j-1](D, D))); // from Di,j-1
			}
		}
		/* Fill the score of the known alignment path */
		for (int j = vpath->start; j <= vpath->end; ++j) {
			for(int i = vpath->from; i <= vpath->to; ++i) {
				int dist = diagnalDist(i, j, vpath->from, vpath->start);
				if(!(dist <= vpath->nIns && dist >= -vpath->nDel))
					continue;
				vs.DP_M(i, j) = E_M_cost(seq.encodeAt(i - 1), j)
						+ BandedHMMP7::min(
								static_cast<double>(vs.DP_M(i, 0) + entryPr_cost(j)), // from B state
								static_cast<double>(vs.DP_M(i - 1, j - 1) + Tmat_cost[j-1](M, M)), // from Mi-1,j-1
								static_cast<double>(vs.DP_I(i - 1, j - 1) + Tmat_cost[j-1](I, M)), // from Ii-1,j-1
								static_cast<double>(vs.DP_D(i - 1, j - 1) + Tmat_cost[j-1](D, M))); // from Di-1,j-1
				vs.DP_I(i, j) = E_I_cost(seq.encodeAt(i - 1), j)
						+ std::min(
								static_cast<double>(vs.DP_M(i - 1, j) + Tmat_cost[j](M, I)), // from Mi-1,j
								static_cast<double>(vs.DP_I(i - 1, j) + Tmat_cost[j](I, I))); // from Ii-1,j
				if(j > 1 && j < K) /* D1 and Dk are retracted */
					vs.DP_D(i, j) = std::min(
							static_cast<double>(vs.DP_M(i, j - 1) + Tmat_cost[j-1](M, D)), // from Mi,j-1
							static_cast<double>(vs.DP_D(i, j - 1) + Tmat_cost[j-1](D, D))); // from Di,j-1
			}
		}
//		 assert(i == vpath->to + 1 && j == vpath->end + 1);
	} /* end of each known path segment */
//	cerr << "known path aligned" << endl;
	/* Dynamic programming of the remaining downstream of the known paths, if any */
	int last_end = vpaths[vpaths.size() - 1].end;
	int last_to = vpaths[vpaths.size() - 1].to;
	int downQLen = L - last_to;
	int down_end = last_end + downQLen * (1 + kMinGapFrac);
	int down_to = last_to + downQLen * (1 + kMinGapFrac);
	if(down_end > K)
		down_end = K;
	if(down_to > L)
		down_to = L;

	for (int j = last_end; j <= down_end; ++j) {
		for (int i = last_to; i <= down_to; ++i) {
			vs.DP_M(i, j) = E_M_cost(seq.encodeAt(i - 1), j) +
					BandedHMMP7::min(
							// from Mi,0, the B state is not possible
							static_cast<double>(vs.DP_M(i - 1, j - 1) + Tmat_cost[j-1](M, M)), // from Mi-1,j-1
							static_cast<double>(vs.DP_I(i - 1, j - 1) + Tmat_cost[j-1](I, M)), // from Ii-1,j-1
							static_cast<double>(vs.DP_D(i - 1, j - 1) + Tmat_cost[j-1](D, M))); // from Di-1,j-1
			vs.DP_I(i, j) = E_I_cost(seq.encodeAt(i - 1), j) +
					std::min(
							static_cast<double>(vs.DP_M(i - 1, j) + Tmat_cost[j](M, I)), // from Mi-1,j
							static_cast<double>(vs.DP_I(i - 1, j) + Tmat_cost[j](I, I))); // from Ii-1,j
			if(j > 1 && j < K) /* D1 and Dk are retracted */
				vs.DP_D(i, j) = std::min(
						static_cast<double>(vs.DP_M(i, j - 1) + Tmat_cost[j-1](M, D)), // from Mi,j-1
						static_cast<double>(vs.DP_D(i, j - 1) + Tmat_cost[j-1](D, D))); // from Di,j-1
		}
	}
//	cerr << "downstream done" << endl;
	vs.S.leftCols(K + 1) = vs.DP_M;; // 0..K columns copied from the calculated DP_M
	vs.S.col(K + 1) = vs.DP_I.col(K);
	//vs.S.col(K + 1).setConstant(inf);
	// add M-E exit costs
	vs.S.leftCols(K + 1).rowwise() += exitPr_cost.transpose();
	vs.S.col(K + 1).array() += Tmat_cost[K](I, M); // IK->E
	vs.S.array() += T_SP_cost(E, C); // add E->C transition
	for (int i = 1; i < L; ++i)
		vs.S.row(i).array() += T_SP_cost(C, C) * (L - i); // add L-i C->C circles
}

BandedHMMP7::ViterbiAlignPath BandedHMMP7::buildAlignPath(const CSLoc& csLoc, int csFrom, int csTo) const {
//	cerr << "csStart:" << csLoc.start << " csEnd:" << csLoc.end << " csFrom:" << csFrom << " csTo:" << csTo <<
//			" CSLen:" << csLoc.CS.length() << " CS:" << csLoc.CS << endl;
	assert(csLoc.isValid(csFrom, csTo));

	/* calculate profile start, end and path */
	int start = 0;
	int end = 0;
	int from = 0;
	int to = 0;
	int nIns = 0;
	int nDel = 0;

	int i = csFrom;
	int j = csLoc.start;
	for(string::const_iterator it = csLoc.CS.begin(); it != csLoc.CS.end(); ++it) {
		int k = getProfileLoc(j); // position on profile
//		cerr << "i:" << i << " j:" << j << " k:" << k << endl;
//		cerr << "vpath.L:" << vpath.L << " vpath.K:" << vpath.K << endl;

		bool nonGap = abc->isSymbol(*it);

		if(from == 0 && nonGap)
			from = i;
		if(nonGap)
			to = i;
		if(k != 0) { // a non-D loc on profile
			if(start == 0) // first time a non-D loc on profile
				start = k;
			end = k; // keep updating
			if(!nonGap) // a deletion
				nDel++;
		}
		else { // a D loc on profile
			if(nonGap) // an insertion
				nIns++;
		}
		j++; // update j
		if(nonGap)
			i++; // update i
	}
//	cerr << "vpath.path:" << vpath.alnPath << endl;
//	cerr << "start: " << start << " end: " << end << endl;
//	cerr << "i:" << i << " j:" << j << " csTo:" << csTo << " csEnd:" << csLoc.end << endl;
	assert(i == csTo + 1 && j == csLoc.end + 1);

	return ViterbiAlignPath(start, end, from, to, nIns, nDel);
}

void BandedHMMP7::buildViterbiTrace(const ViterbiScores& vs, ViterbiAlignTrace& vtrace) const {
	MatrixXd::Index minRow, minCol;
	vtrace.minScore = vs.S.minCoeff(&minRow, &minCol);
	if(vtrace.minScore == inf)
		return; // return an invalid VTrace

	/* do trace back in the vScore matrix */
	char s = minCol <= K ? 'M' : 'I'; // exiting state either M1..K or IK
	int i = minRow;
	int j = minCol <= K ? minCol : K;

//	vtrace.alnStart = minCol <= K ? minCol : K;
	vtrace.alnEnd = minCol <= K ? minCol : K;
	vtrace.alnTo = minRow;

	vtrace.alnTrace.push_back('E'); // ends with E
	while(i >= 1 && j >= 0) {
//		cerr << "i: " << i << " j: " << j << " s: " << s << endl;
		vtrace.alnTrace.push_back(s);
		// update the status
		if(s == 'M') {
			s = j > 1 ? BandedHMMP7::whichMin(
						static_cast<double> (vs.DP_M(i, 0) + entryPr_cost(j)), /* from B-state */
						static_cast<double> (vs.DP_M(i - 1, j - 1) + Tmat_cost[j-1](M, M)), /* from M(i-1,j-1) */
						static_cast<double> (vs.DP_I(i - 1, j - 1) + Tmat_cost[j-1](I, M)), /* from I(i-1,j-1) */
						static_cast<double> (vs.DP_D(i - 1, j - 1) + Tmat_cost[j-1](D, M))) : /* from D(i-1,j-1) */
					BandedHMMP7::whichMin(
							static_cast<double> (vs.DP_M(i, 0) + entryPr_cost(j)), /* from B-state */
							static_cast<double> (vs.DP_I(i - 1, j - 1) + Tmat_cost[j-1](I, M)), /* from I(i-1,j-1) */
							"BI");
			i--;
			j--;
		}
		else if(s == 'I') {
			vtrace.alnFrom--;
			s = j > 0 ? BandedHMMP7::whichMin(
					static_cast<double> (vs.DP_M(i - 1, j) + Tmat_cost[j](M, I)), /* from M(i-1,j) */
					static_cast<double> (vs.DP_I(i - 1, j) + Tmat_cost[j](I, I)), /* from I(i-1,j) */
					"MI") :
					BandedHMMP7::whichMin(
										static_cast<double> (vs.DP_M(i, 0) + Tmat_cost[0](M, I)), /* from B aka M(0) */
										static_cast<double> (vs.DP_I(i - 1, j) + Tmat_cost[j](I, I)), /* from I(i-1,j) */
										"BI");
			i--;
		}
		else if(s == 'D') {
			s = BandedHMMP7::whichMin(
					static_cast<double> (vs.DP_M(i, j - 1) + Tmat_cost[j-1](M, D)), /* from M(i,j-1) */
					static_cast<double> (vs.DP_D(i, j - 1) + Tmat_cost[j-1](D, D)), /* from D(i,j-1) */
					"MD");
			j--;
		}
		else /* B */
			break;
	} /* end of while */

	vtrace.alnStart = j + 1; /* 1-based */
	vtrace.alnFrom = i + 1;  /* 1-based */

	assert(vtrace.alnStart > 0 && vtrace.alnFrom > 0);
	if(*vtrace.alnTrace.rbegin() != 'B')
		vtrace.alnTrace.push_back('B');
	reverse(vtrace.alnTrace.begin(), vtrace.alnTrace.end()); // reverse the alnPath string
}

BandedHMMP7::HmmAlignment BandedHMMP7::buildGlobalAlign(const PrimarySeq& seq,
		const ViterbiScores& vs, const ViterbiAlignTrace& vtrace) const {
	assert(seq.length() == vs.L);

	HmmAlignment aln;

	const string& seqN = seq.getSeq().substr(0, vtrace.alnFrom - 1); /* N' of unaligned seq, might be empty */
	const string& seqC = seq.getSeq().substr(vtrace.alnTo, L - vtrace.alnTo); /* C' of unaligned seq, might be empty */

	int csStart = profile2CSIdx[vtrace.alnStart]; /* 1-based */
	int csEnd = profile2CSIdx[vtrace.alnEnd]; /* 1-based */

	int i = 0; /* 1-based position on CS */
	int j = 0; /* 1-based position on seq */
	int k = 0; /* 1-based position on HMM */

	string insert;
	for(string::const_iterator state = vtrace.alnTrace.begin(); state != vtrace.alnTrace.end(); ++state) {
//		fprintf(stderr, "i:%d j:%d k:%d cs:%d state:%c aln:%s\n", state - vtrace.alnTrace.begin(), j, k, profile2CSIdx[k], *state, aln.c_str());
		switch(*state) {
		case 'B':
			aln.align.append(getPaddingSeq(csStart - 1, seqN, PAD_SYM, RIGHT)); /* right aligned N' padding */
			i = csStart;
			j = vtrace.alnFrom;
			k = vtrace.alnStart;
			break;
		case 'M':
			if(k > 1 && state - vtrace.alnTrace.begin() > 1 && profile2CSIdx[k] - profile2CSIdx[k - 1] > 1) /* there are non-CS pos before 'M' */
				/* fill in the gap with either insert or GAP */
				aln.align.append(getPaddingSeq(profile2CSIdx[k] - profile2CSIdx[k - 1] - 1, insert, GAP_SYM, JUSTIFIED)); /* justified aligned gap padding */
			insert.clear();
			aln.align.push_back(seq.charAt(j - 1));
			j++;
			k++;
			break;
		case 'I':
			insert.clear();
			while(*state == 'I') { /* process all insertions in once */
				insert.push_back(::tolower(seq.charAt(j - 1)));
				j++;
				state++;
			}
			state--; // rewind
			break;
		case 'D':
			assert(insert.empty()); /* no I possible before D */
			if(k > 1 && profile2CSIdx[k] - profile2CSIdx[k - 1] > 1) /* there are non-CS pos before 'D' */
				/* fill in the gap with either insert or GAP */
				aln.align.append(profile2CSIdx[k] - profile2CSIdx[k - 1] - 1, GAP_SYM);
			aln.align.push_back(GAP_SYM);
			k++;
			break;
		case 'E':
			assert(j == vtrace.alnTo + 1);
			aln.align.append(getPaddingSeq(L - csEnd, seqC, PAD_SYM, LEFT)); /* left aligned C' padding */
			break;
		default:
			cerr << "Unexpected align path state '" << *state << "' found" << endl;
			break;
		}
	}

	assert(aln.align.length() == L);
	aln.K = K;
	aln.L = L;
	aln.seqStart = vtrace.alnFrom;
	aln.seqEnd = vtrace.alnTo;
	aln.hmmStart = vtrace.alnStart;
	aln.hmmEnd = vtrace.alnEnd;
	aln.csStart = csStart;
	aln.csEnd = csEnd;
	aln.cost = vtrace.minScore;
	return aln;
}

void BandedHMMP7::wingRetract() {
	if(wingRetracted) // already wing-retracted
		return;
	/* retract entering costs */
	/* increase the B->Mj entry cost by adding the chain B->D1->D2->...->Dj-1->Mj */
	for(MatrixXd::Index j = 2; j <= K; ++j) {
		double cost = 0; // additional retract cost in log-scale
		cost += Tmat_cost[0](M, D); // B->D1 (M0->D1)
		for(MatrixXd::Index i = 1; i < j - 1; ++i)
			cost += Tmat_cost[i](D, D); // Di->Di+1
		cost += Tmat_cost[j-1](D, M); // Dj-1->Mj
		assert(cost > 0);
		entryPr(j) += ::exp(-cost); // retract B->D1->D2...Dj-1->Mj to B->Mj
		if(entryPr(j) > 1)
			entryPr(j) = 1;
	}
	/* retract exiting costs */
	/* increase the Mj->E cost by adding the chain Mj->Dj+1->Dj+2->...->DK->E */
	for(MatrixXd::Index i = 1; i <= K - 1; ++i) {
		double cost = 0; // additional retract cost in log-scale
		cost += Tmat_cost[i](M, D); // Mj -> Di+1
		for(MatrixXd::Index j = i + 1; j < K; ++j)
			cost += Tmat_cost[j](D, D); // Dj->Dj+1
		cost += Tmat_cost[K](D, M); // DK -> E (DK->MK+1)
		assert(cost > 0);
		exitPr(i) += ::exp(-cost); // retract Mj->Dj+1->Dj+2...->DK->E to Mj->E
		if(exitPr(i) > 1)
			exitPr(i) = 1;
	}
	/* set transition matrices */
	/* reset log transition matrices */
//	cerr << "entry before retract: " << entryPr_cost.transpose() << endl;
	entryPr_cost = -entryPr.array().log();
	exitPr_cost = -exitPr.array().log();
//	cerr << "entry after retract: " << entryPr_cost.transpose() << endl;

	wingRetracted = true;
}

double RelativeEntropyTargetFunc::operator()(double x) {
	BandedHMMP7 hmm2(hmm); // use a copy so original hmm won't be affected

	if(x > hmm2.effN) // do not scale up
		return 0;

	hmm2.effN = x;
	hmm2.scale(hmm2.effN / hmm2.nSeq);
	hmm2.estimateParams(prior);
	double relEnt = hmm2.meanRelativeEntropy();
//	cerr << "current effN: " << x << " ere: " << relEnt << endl;
//	return hmm.meanRelativeEntropy() - ere;
	return relEnt - ere;
}

string BandedHMMP7::getPaddingSeq(int L, const string& insert, char padCh, padding_mode mode) {
	if(insert.empty())
		return getPaddingSeq(L, padCh);

	string pad;
	switch(mode) {
	case LEFT:
		if(insert.length() >= L)
			pad.append(insert.substr(0, L));
		else {
			pad.append(insert);
			pad.append(L - insert.length(), padCh);
		}
		break;
	case RIGHT:
		if(insert.length() >= L)
			pad.append(insert.substr(insert.length() - L, L));
		else {
			pad.append(L - insert.length(), padCh);
			pad.append(insert);
		}
		break;
	case MIDDLE:
		if(insert.length() >= L)
			pad.append(insert.substr((insert.length() - L) / 2, L));
		else {
			pad.append(static_cast<int> (::floor((L - insert.length()) / 2.0)), padCh);
			pad.append(insert);
			pad.append(static_cast<int> (::ceil((L - insert.length()) / 2.0)), padCh);
		}
		break;
	case JUSTIFIED:
		if(insert.length() >= L) {
			pad.append(insert.substr(0, static_cast<int> (::floor(L / 2.0))));
			pad.append(insert.substr(insert.length() - static_cast<int> (::ceil(L / 2.0)), static_cast<int> (::ceil(L / 2.0))));
		}
		else {
			pad.append(insert.substr(0, static_cast<int> (::floor(insert.length() / 2.0))));
			pad.append(L - insert.length(), padCh);
			pad.append(insert.substr(0, static_cast<int> (::ceil(insert.length() / 2.0))));
		}
		break;
	default:
		pad.append(L, padCh);
		break;
	}

	assert(pad.length() == L);
	return pad;
}

BandedHMMP7::HmmAlignment& BandedHMMP7::HmmAlignment::merge(const HmmAlignment& other) {
	if(isCompatitable(other)) {
		/* merge seq loc */
		if(other.seqStart < seqStart)
			seqStart = other.seqStart;
		if(other.seqEnd > seqEnd)
			seqEnd = other.seqEnd;
		/* merge HMM loc */
		if(other.hmmStart < hmmStart)
			hmmStart = other.hmmStart;
		if(other.hmmEnd > hmmEnd)
			hmmEnd = other.hmmEnd;
		/* merge CS loc */
		if(other.csStart < csStart)
			csStart = other.csStart;
		if(other.csEnd > csEnd)
			csEnd = other.csEnd;
		/* add cost */
		cost += other.cost;
		/* merge aligned seq */
		for(string::size_type i = 0; i < L; ++i)
			if(align[i] == BandedHMMP7::PAD_SYM && other.align[i] != BandedHMMP7::PAD_SYM) /* this align has priority */
				align[i] = other.align[i];
	}
	return *this;
}

ostream& operator<<(ostream& out, const BandedHMMP7::HmmAlignment& hmmAln) {
	out << hmmAln.seqStart << "\t" << hmmAln.seqEnd << "\t" <<
			hmmAln.hmmStart << "\t" << hmmAln.hmmEnd << "\t" <<
			hmmAln.csStart << "\t" << hmmAln.csEnd << "\t" <<
			hmmAln.cost << "\t" << hmmAln.align;
	return out;
}

istream& operator>>(istream& in, BandedHMMP7::HmmAlignment& hmmAln) {
	in >> hmmAln.seqStart >> hmmAln.seqEnd >>
	hmmAln.hmmStart >> hmmAln.hmmEnd >>
	hmmAln.csStart >> hmmAln.csEnd >>
	hmmAln.cost >> hmmAln.align;
	return in;
}

} /* namespace HmmUFOtu */
} /* namespace EGriceLab */
