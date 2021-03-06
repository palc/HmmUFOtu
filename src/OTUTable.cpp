/*
 * OTUTable.cpp
 *
 *  Created on: Jul 11, 2017
 *      Author: zhengqi
 */

#include <ctime>
#include <cassert>
#include <algorithm>
#include <boost/lexical_cast.hpp>
#include <boost/random/random_number_generator.hpp> /* an adapter between randome_number_generator and uniform_random_number_generator */
#include <Eigen/Dense>
#include "HmmUFOtuConst.h"
#include "OTUTable.h"

namespace EGriceLab {
namespace HmmUFOtu {

using namespace std;
using namespace Eigen;

const IOFormat OTUTable::dblTabFmt(FullPrecision, DontAlignCols, "\t", "\n", "", "", "");
const IOFormat OTUTable::fltTabFmt(FullPrecision, DontAlignCols, "\t", "\n", "", "", "");
OTUTable::RNG OTUTable::rng(time(NULL)); /* initiate RNG with random seed */

bool OTUTable::addSample(const string& sampleName) {
	if(hasSample(sampleName))
		return false;

	const size_t N = otuMetric.cols();
	samples.push_back(sampleName);
	otuMetric.conservativeResize(Eigen::NoChange, N + 1);
	otuMetric.col(N).setZero();

	return true;
}


bool OTUTable::removeSample(size_t j) {
	if(!(j >= 0 && j < numSamples()))
		return false;

	const size_t N = otuMetric.cols();
	samples.erase(samples.begin() + j);
	MatrixXd oldMetric(otuMetric); /* copy the old values */
	otuMetric.resize(Eigen::NoChange, N - 1);
	for(MatrixXd::Index n = 0, k = 0; n < N; ++n) {
		if(n != j) { /* not the deleted column */
			otuMetric.col(k) = oldMetric.col(n);
			k++;
		}
	}

	return true;
}


bool OTUTable::addOTU(const string& otuID, const string& taxon, const RowVectorXd& count) {
	if(hasOTU(otuID))
		return false;

	const size_t M = otuMetric.rows();
	otus.push_back(otuID);
	otu2Taxon[otuID] = taxon;
	otuMetric.conservativeResize(M + 1, Eigen::NoChange);
	otuMetric.row(M) = count;

	return true;
}

bool OTUTable::removeOTU(size_t i) {
	if(!(i >= 0 && i < numOTUs()))
		return false;

	const size_t M = otuMetric.rows();
	otu2Taxon.erase(otus[i]); /* remove taxon first */
	otus.erase(otus.begin() + i); /* remove the actual OTU */
	MatrixXd oldMetric(otuMetric); /* copy the old values */
	otuMetric.resize(M - 1, Eigen::NoChange);
	for(MatrixXd::Index m = 0, k = 0; m < M; ++m) {
		if(m != i) { /* not the deleted row */
			otuMetric.row(k) = oldMetric.row(m);
			k++;
		}
	}

	return true;
}

void OTUTable::pruneSamples(size_t min) {
	if(min == 0)
		return;

	const size_t N = numSamples();
	/* remove samples backwards */
	for(size_t j = N; j > 0; --j) {
		if(numSampleReads(j - 1) < min)
			removeSample(j - 1);
	}
}

void OTUTable::pruneOTUs(size_t min) {
	const size_t M = numOTUs();
	/* remove samples backwards */
	for(size_t i = M; i > 0; --i) {
		double nRead = numOTUReads(i - 1);
		if(min > 0 && nRead < min || min == 0 && nRead == 0)
			removeOTU(i - 1);
	}
}

void OTUTable::normalizeConst(double Z) {
	assert(Z >= 0);
	if(isEmpty() || (otuMetric.array() == 0).all()) /* empty or all zero metric */
		return;
	if(Z == 0)
		Z = otuMetric.colwise().sum().maxCoeff(); /* use max column sum as constant */

	const size_t N = otuMetric.cols();
	RowVectorXd norm = otuMetric.colwise().sum() / Z;
	for(MatrixXd::Index j = 0; j < N; ++j)
		otuMetric.col(j) /= norm(j);
}

istream& OTUTable::loadTable(istream& in) {
	clear(); /* clear old data */
	/* input header */
	string line;
	size_t M = 0;
	size_t N = 0;
	while(std::getline(in, line)) {
		vector<string> fields;
		boost::split(fields, line, boost::is_any_of("\t"));
		if(fields[0] == "otuID") { /* header line */
			N = fields.size() - 2;
			/* update samples */
			samples.resize(N);
			std::copy(fields.begin() + 1, fields.end() - 1, samples.begin());
			otuMetric.resize(0, N);
		}
		else { /* value line */
			if(fields.size() != N + 2)
				continue;
			bool isNew = addOTU(fields[0], fields[fields.size() - 1]); /* add a new OTU */
			if(!isNew)
				continue;
			otuMetric.conservativeResize(M + 1, Eigen::NoChange);
			for(size_t j = 0; j < N; ++j)
				otuMetric(M, j) = boost::lexical_cast<double> (fields[j + 1]);
			M++;
		}
	}

	return in;
}

ostream& OTUTable::saveTable(ostream& out) const {
	/* output header */
	out << "otuID\t" << boost::join(samples, "\t") << "\ttaxonomy" << endl;

	/* output each OTU */
	const size_t M = numOTUs();
	for(size_t i = 0; i < M; ++i)
		out << otus[i] << "\t" << otuMetric.row(i).format(fltTabFmt) << "\t" << otu2Taxon.at(otus[i]) << endl;

	return out;
}

void OTUTable::subsetUniform(size_t min) {
	for(int j = 0; j < numSamples(); ++j) {
		double sampleTotal = numSampleReads(j);
		if(sampleTotal <= min) /* not enough reads to subset */
			continue;

		/* generate an sampling index with length M */
		std::vector<bool> otuIdx(static_cast<size_t> (sampleTotal), false); /* use the efficient std::vector<bool>, default all false */
		fill_n(otuIdx.begin(), min, true);
		boost::random_number_generator<RNG, size_t> gen(rng);
		boost::random_shuffle(otuIdx, gen);
		/* subset reads in OTUs without replacement using the random index */
		for(size_t i = 0, k = 0; i < numOTUs(); ++i) { /* k is the start index of current OTU */
			size_t N = static_cast<size_t> (otuMetric(i, j));
			otuMetric(i, j) = std::count(otuIdx.begin() + k, otuIdx.begin() + k + N, true);
			assert(otuMetric(i, j) <= N);
			k += N;
		}
	}
}

void OTUTable::subsetMultinom(size_t min) {
	const size_t M = numOTUs();
	double *otuPr = new double[M]; /* raw read sample probabilities */
	Map<VectorXd> otuPrMap(otuPr, M); /* use a map to access indirectly */
	otuPrMap.setOnes(); /* use all equal probs by default */
	ReadDistrib rdist(otuPr, otuPr + M); /* construct the discrete distribution */

	for(int j = 0; j < numSamples(); ++j) {
		double sampleTotal = numSampleReads(j);
		if(sampleTotal <= min) /* not enough reads to subset */
			continue;

		/** reset rdist probabilities according to current counts */
		otuPrMap = otuMetric.col(j);
		rdist.param(ReadParam(otuPr, otuPr + M));
		/* sample min reads */
		VectorXd sampled = VectorXd::Zero(M);
		for(size_t m = 0; m < min; ++m)
			sampled(rdist(rng))++;
		otuMetric.col(j) = sampled;
	}
	delete[] otuPr;
}

} /* namespace HmmUFOtu */
} /* namespace EGriceLab */
