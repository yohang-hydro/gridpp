#ifndef DOWNSCALER_NEAREST_NEIGHBOUR_H
#define DOWNSCALER_NEAREST_NEIGHBOUR_H
#include <map>
#include "Downscaler.h"
#include "../Variable.h"
#include "../Util.h"
#include "../Field.h"
class File;
class DownscalerNearestNeighbour : public Downscaler {
   public:
      DownscalerNearestNeighbour(Variable::Type iVariable, const Options& iOptions);
      static std::string description();
      static void downscaleField(const Field& iInput, Field& iOutput,
            const vec2& iInputLats, const vec2& iInputLons,
            const vec2& iOutputLats, const vec2& iOutputLons,
            const vec2Int& nearestI, const vec2Int& nearestJ);
      static vec2 downscaleVec(const vec2& iInput, 
            const vec2& iInputLats, const vec2& iInputLons,
            const vec2& iOutputLats, const vec2& iOutputLons,
            const vec2Int& nearestI, const vec2Int& nearestJ);
      std::string name() const {return "nearestNeighbour";};
   private:
      void downscaleCore(const File& iInput, File& iOutput) const;
};
#endif
