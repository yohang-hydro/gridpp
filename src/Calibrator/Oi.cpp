#include "Oi.h"
#include "../Util.h"
#include "../Parameters.h"
#include "../File/File.h"
#include "../Downscaler/Downscaler.h"
#include <math.h>
#include <armadillo>
#include "/home/thomasn/local/include/armadillo"

CalibratorOi::CalibratorOi(Variable iVariable, const Options& iOptions):
      Calibrator(iVariable, iOptions),
      mVLength(100),
      mHLength(30000),
      mMu(0.25),
      mMinObs(3),
      mSort(true),
      mMinRho(0.0013),
      mObsOnly(false),
      mBiasVariable(""),
      mSigma(1),
      mDelta(1),
      mX(Util::MV),
      mY(Util::MV),
      mMaxLocations(20),
      mNumVariable(""),
      mUseMeanBias(false),
      mMaxBytes(6.0 * 1024 * 1024 * 1024),
      mGamma(0.9) {
   iOptions.getValue("bias", mBiasVariable);
   iOptions.getValue("d", mHLength);
   iOptions.getValue("h", mVLength);
   iOptions.getValue("maxLocations", mMaxLocations);
   iOptions.getValue("sort", mSort);
   iOptions.getValue("sigma", mSigma);
   iOptions.getValue("delta", mDelta);
   iOptions.getValue("obsOnly", mObsOnly);
   iOptions.getValue("minObs", mMinObs);
   iOptions.getValue("x", mX);
   iOptions.getValue("y", mY);
   iOptions.getValue("minRho", mMinRho);
   iOptions.getValue("maxBytes", mMaxBytes);
   iOptions.getValue("numVariable", mNumVariable);
   iOptions.getValue("useMeanBias", mUseMeanBias);
}

// Set up convenient functions for debugging in gdb
template<class Matrix>
void print_matrix(Matrix matrix) {
       matrix.print(std::cout);
}
typedef arma::mat mattype;
typedef arma::vec vectype;
typedef arma::cx_mat cxtype;
template void print_matrix<mattype>(mattype matrix);
template void print_matrix<cxtype>(cxtype matrix);

bool CalibratorOi::calibrateCore(File& iFile, const ParameterFile* iParameterFile) const {
   int nY = iFile.getNumY();
   int nX = iFile.getNumX();
   int nEns = iFile.getNumEns();
   int nTime = iFile.getNumTime();
   vec2 lats = iFile.getLats();
   vec2 lons = iFile.getLons();
   vec2 elevs = iFile.getElevs();

   // Check if this method can be applied
   bool hasValidGridpoint = false;
   for(int y = 0; y < nY; y++) {
      for(int x = 0; x < nX; x++) {
         if(Util::isValid(lats[y][x]) && Util::isValid(lons[y][x]) && Util::isValid(elevs[y][x])) {
            hasValidGridpoint = true;
         }
      }
   }
   if(!hasValidGridpoint) {
      Util::warning("There are no gridpoints with valid lat/lon/elev values. Skipping oi...");
      return false;
   }

   std::vector<Location> obsLocations = iParameterFile->getLocations();
   if(iParameterFile->getNumParameters() != 2) {
      std::stringstream ss;
      ss << "Parameter file has " << iParameterFile->getNumParameters() << " parameters, not 2";
      Util::error(ss.str());
   }

   // Retrieve parameters
   std::vector<float> ci(obsLocations.size());
   std::vector<float> obs(obsLocations.size());
   std::vector<float> obselevs(obsLocations.size());
   for(int i = 0; i < obsLocations.size(); i++) {
      Parameters parameters = iParameterFile->getParameters(0, obsLocations[i]);
      obs[i] = parameters[0];
      obselevs[i] = obsLocations[i].elev();
      ci[i] = parameters[1];
   }
   float sigma = mSigma;
   int S = obsLocations.size();
   // Find the spacing between each grid
   float gridSize = Util::getDistance(lats[0][0], lons[0][0], lats[1][0], lons[1][0]);
   std::stringstream ss;
   ss << "Grid size: " << gridSize << " m";
   Util::info(ss.str());

   // Loop over each observation, find the nearest gridpoint and place the obs into all gridpoints
   // in the vicinity of the nearest neighbour. This is only meant to be an approximation, but saves
   // considerable time instead of doing a loop over each grid point and each observation.

   // Store the indicies (into the obsLocations array) that a gridpoint has available
   std::vector<std::vector<std::vector<int> > > obsIndices; // Y, X, obs indices
   std::vector<float> obsY(S);
   std::vector<float> obsX(S);
   obsIndices.resize(nY);
   for(int y = 0; y < nY; y++) {
      obsIndices[y].resize(nX);
   }

   // Spread each observation out to this many gridpoints from the nearest neighbour
   int radius = 3.64 * mHLength / gridSize;

   // getchar();
   // 700 MB

   // When large radiuses are used, the process becomes memory-intensive. Try to fail here
   // if we expect to use more memory than desired. The true memory is roughly
   // 1 GB + expectedBytes * F
   int bytesPerValue = 4;
   float expectedBytes = float(radius * radius) * 4 * bytesPerValue * S;
   std::cout << "Expected MB: " << 1000 + expectedBytes / 1024 / 1024 << std::endl;
   if(Util::isValid(mMaxBytes) && expectedBytes > mMaxBytes) {
      std::stringstream ss;
      ss << "Expected size (" << expectedBytes / 1024 / 1024 << " GB) is greater than "
         << float(mMaxBytes) / 1024 / 1024 << " GB";
      Util::error(ss.str());
   }

   double time_s = Util::clock();
   KDTree searchTree(iFile.getLats(), iFile.getLons());
   int count = 0;
   for(int i = 0; i < S; i++) {
      if(i % 1000 == 0) {
         std::stringstream ss;
         ss << i;
         Util::info(ss.str());
      }
      if(Util::isValid(obs[i])) {
         int Y, X;
         searchTree.getNearestNeighbour(obsLocations[i].lat(), obsLocations[i].lon(), Y, X);
         for(int y = std::max(0, Y - radius); y < std::min(nY, Y + radius); y++) {
            for(int x = std::max(0, X - radius); x < std::min(nX, X + radius); x++) {
               if(mSort || obsIndices[y][x].size() < mMaxLocations) {
                  obsIndices[y][x].push_back(i);
                  count ++;
               }
            }
         }
         obsY[i] = Y;
         obsX[i] = X;
      }
   }
   double time_e = Util::clock();
   std::cout << "Assigning locations " << time_e - time_s << std::endl;

   // Loop over offsets
   for(int t = 0; t < nTime; t++) {
      FieldPtr field = iFile.getField(mVariable, t);
      FieldPtr output = iFile.getEmptyField();
      FieldPtr bias;
      if(iFile.hasVariable(mBiasVariable))
         bias = iFile.getField(mBiasVariable, t);
      FieldPtr num;
      if(mNumVariable != "")
         num = iFile.getField(mBiasVariable, t);

#if 0
      // Parallelize both x and y. Can be useful if there is load imbalance on a particular
      // y slice.
      #pragma omp parallel for
      for(int yx = 0; yx < nY*nX; yx++) {
         int y = yx / nX;
         int x = yx % nX;
         {
#else
      #pragma omp parallel for
         for(int x = 0; x < nX; x++) {
      for(int y = 0; y < nY; y++) {
#endif
            float lat = lats[y][x];
            float lon = lons[y][x];
            float elev = elevs[y][x];
            std::vector<int> useLocations0 = obsIndices[y][x];

            // Reduce the list of locations
            std::vector<int> useLocations;
            useLocations.reserve(useLocations0.size());
            std::vector<std::pair<float,int> > rhos;
            rhos.reserve(useLocations0.size());
            for(int i = 0; i < useLocations0.size(); i++) {
               int index = useLocations0[i];
               float hdist = Util::getDistance(obsLocations[index].lat(), obsLocations[index].lon(), lat, lon, true);
               float vdist = obsLocations[index].elev() - elev;
               float rho = calcRho(hdist, vdist);
               int X = obsX[index];
               int Y = obsY[index];
               // Only include observations that are within the domain
               if(X > 0 && X < lats[0].size() && Y > 0 && Y < lats.size()) {
                  if(rho > mMinRho) {
                     rhos.push_back(std::pair<float,int>(rho, i));
                  }
               }
            }
            if(rhos.size() > mMaxLocations) {
               // If sorting is enabled and we have too many locations, then only keep the best ones based on rho.
               // Otherwise, just use the last locations added
               if(mSort) {
                  std::sort(rhos.begin(), rhos.end(), Util::sort_pair_first<float,int>());
               }
               for(int i = 0; i < mMaxLocations; i++) {
                  // The best values start at the end of the array
                  int index = rhos[rhos.size() - 1 - i].second;
                  useLocations.push_back(useLocations0[index]);
               }
            }
            else {
               for(int i = 0; i < rhos.size(); i++) {
                  int index = rhos[i].second;
                  useLocations.push_back(useLocations0[index]);
               }
            }

            int nObs = useLocations.size();

            if(mObsOnly) {
               // Here we don't run the OI algorithm but instead just use the median
               // of all observations
               if(nObs < mMinObs) {
                  // If we have too few observations though, then use the background
                  for(int e = 0; e < nEns; e++) {
                     (*output)(y, x, e) = (*field)(y, x, e);
                  }
               }
               else {
                  std::vector<float> currObs(nObs, Util::MV);
                  for(int i = 0; i < nObs; i++) {
                     int index = useLocations[i];
                     currObs[i] = obs[index];
                  }
                  float value = Util::calculateStat(currObs, Util::StatTypeQuantile, 0.5);
                  for(int e = 0; e < nEns; e++) {
                     (*output)(y, x, e) = value;
                  }
               }
               continue;
            }

            if(nObs < mMinObs) {
               for(int e = 0; e < nEns; e++) {
                  (*output)(y, x, e) = (*field)(y, x, e);
               }
               continue;
            }

            vectype currObs(nObs);
            vectype currElev(nObs);
            for(int i = 0; i < useLocations.size(); i++) {
               int index = useLocations[i];
               float curr = obs[index];
               float elev = obselevs[index];
               currObs[i] = curr;
               currElev[i] = elev;
            }

            // Compute Rinv
            mattype Rinv(nObs, nObs, arma::fill::zeros);
            for(int i = 0; i < nObs; i++) {
               int index = useLocations[i];
               float dist = Util::getDistance(obsLocations[index].lat(), obsLocations[index].lon(), lat, lon, true);
               float vdist = obsLocations[index].elev() - elev;
               float rho = calcRho(dist, vdist);
               float r = sigma * ci[index];
               Rinv(i, i) = 1 / r * rho;
            }

            // Compute Y (model at obs-locations)
            mattype Y(nObs, nEns);
            vectype Yhat(nObs);

            for(int i = 0; i < nObs; i++) {
               // Use the nearest neighbour for this location
               float total = 0;
               int count = 0;
               for(int e = 0; e < nEns; e++) {
                  float value = (*field)(obsY[i], obsX[i], e);
                  if(Util::isValid(value)) {
                     Y(i, e) = value;
                     total += value;
                     count++;
                  }
               }
               // assert(count > 0);
               float mean = total / count;
               Yhat(i) = mean;
               for(int e = 0; e < nEns; e++) {
                  Y(i, e) -= mean;
               }
            }

            // Compute C matrix
            // k x S * S x S
            mattype C(nEns, nObs);
            C = Y.t() * Rinv;

            mattype Pinv(nEns, nEns);
            float diag = 1 / mDelta / (1 + mGamma) * (nEns - 1);
            Pinv = C * Y + diag * arma::eye<mattype>(nEns, nEns);
            float cond = arma::rcond(Pinv);
            if(cond <= 0) {
               std::stringstream ss;
               ss << "Condition number of " << cond << ". Using raw values";
               Util::warning(ss.str());
               for(int e = 0; e < nEns; e++) {
                  (*output)(y, x, e) = (*field)(y, x, e); // Util::MV;
               }
               continue;
            }

            mattype P = arma::inv(Pinv);
            cxtype Wcx(nEns, nEns);
            bool status = arma::sqrtmat(Wcx, (nEns - 1) * P);
            if(!status) {
               std::cout << "Near singular matrix for sqrtmat:" << std::endl;
               std::cout << "Lat: " << lat << std::endl;
               std::cout << "Lon: " << lon << std::endl;
               std::cout << "Elev: " << elev << std::endl;
               std::cout << "P" << std::endl;
               print_matrix<mattype>(P);
               std::cout << "Y:" << std::endl;
               print_matrix<mattype>(Y);
               std::cout << "currObs:" << std::endl;
               print_matrix<mattype>(currObs);
               std::cout << "Yhat" << std::endl;
               print_matrix<mattype>(Yhat);
            }

            mattype W = arma::real(Wcx);
            if(W.n_rows == 0) {
               std::stringstream ss;
               ss << "Could not find the real part of W. Using raw values.";
               Util::warning(ss.str());
               for(int e = 0; e < nEns; e++) {
                  (*output)(y, x, e) = (*field)(y, x, e);
               }
               continue;
            }

            // Compute PC
            mattype PC(nEns, nObs);
            PC = P * C;

            // Compute w
            vectype w(nEns);
            w = PC * (currObs - Yhat);

            // Add w to W. TODO: Is this done correctly?
            for(int e = 0; e < nEns; e++) {
               for(int e2 = 0; e2 < nEns; e2 ++) {
                  W(e, e2) = W(e, e2) + w(e) ;
               }
            }

            // Compute X (perturbations about model mean)
            vectype X(nEns);
            float total = 0;
            int count = 0;
            for(int e = 0; e < nEns; e++) {
               float value = (*field)(y, x, e);
               if(Util::isValid(value)) {
                  X(e) = value;
                  total += value;
                  count++;
               }
               else {
                  std::cout << "Invalid value " << y << " " << x << " " << e << std::endl;
               }
            }
            float mean = total / count;
            for(int e = 0; e < nEns; e++) {
               X(e) -= mean;
            }

            // Compute analysis
            for(int e = 0; e < nEns; e++) {
               float total = 0;
               for(int k = 0; k < nEns; k++) {
                  total += X(k) * W(k, e);
               }
               if(!mUseMeanBias)
                  (*output)(y, x, e) = (*field)(y, x, e) + total;
               else {
                  float meanBias = arma::mean(currObs - Yhat);
                  (*output)(y, x, e) = (*field)(y, x, e) + meanBias;
               }

               if(mNumVariable != "")
                  (*num)(y, x, e) = nObs;

               // Write debugging information
               if(x == mX && y == mY) {
                  std::cout << "Lat: " << lat << std::endl;
                  std::cout << "Lon: " << lon << std::endl;
                  std::cout << "Elev: " << elev << std::endl;
                  std::cout << "P" << std::endl;
                  print_matrix<mattype>(P);
                  std::cout << "PC" << std::endl;
                  print_matrix<mattype>(PC);
                  std::cout << "W" << std::endl;
                  print_matrix<mattype>(W);
                  std::cout << "w" << std::endl;
                  print_matrix<mattype>(w);
                  std::cout << "Y:" << std::endl;
                  print_matrix<mattype>(Y);
                  std::cout << "Yhat" << std::endl;
                  print_matrix<mattype>(Yhat);
                  std::cout << "currObs" << std::endl;
                  print_matrix<mattype>(currObs);
                  std::cout << "X" << std::endl;
                  print_matrix<mattype>(X);
                  std::cout << "elevs" << std::endl;
                  print_matrix<mattype>(currElev);
                  std::cout << "Analaysis:" << std::endl;
                  print_matrix<mattype>(X.t() * W);
               }
            }
         }
      }
      std::cout << "Adding" << std::endl;
      iFile.addField(output, mVariable, t);
      if(mNumVariable != "")
         iFile.addField(num, Variable(mNumVariable), t);
   }
   return true;
}

float CalibratorOi::calcRho(float iHDist, float iVDist) const {
   float h = (iHDist/mHLength);
   float v = (iVDist/mVLength);
   float rho = exp(-0.5 * h * h) * exp(-0.5 * v * v);
   return rho;
}

std::string CalibratorOi::description() {
   std::stringstream ss;
   ss << Util::formatDescription("-c oi","Spreads bias in space by using kriging. A parameter file is required, which must have one column with the bias.")<< std::endl;
   return ss.str();
}
