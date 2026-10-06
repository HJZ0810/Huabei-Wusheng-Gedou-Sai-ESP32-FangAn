/**
 * @file    calibration_model.h
 * @brief   校验标定结果归属，并根据外部实测量生成待保存配置。
 * @details 调用方在配置仲裁窗口内传入快照；本函数不写 NVS、不认可动作实测精度。
 *          结果编号、上电会话和配置代次必须同时匹配，避免旧页面覆盖新参数。
 */
#pragma once
#include "types.h"
#include <cmath>
#include <cstring>
namespace bot {
inline bool prepareCalibration(const Config& current,const Telemetry& tm,const char* mode,
    uint32_t id,uint32_t sessionId,uint32_t revision,uint32_t appliedId,float measured,Config& next) {
  const bool odo=!std::strcmp(mode,"odo"),legacyTurn=!std::strcmp(mode,"turn");
  const bool left=!std::strcmp(mode,"turn_left"),right=!std::strcmp(mode,"turn_right");
  if(!(odo||legacyTurn||left||right) || !tm.resultReady || !id || id!=tm.resultId || id==appliedId ||
      !sessionId || sessionId!=tm.calibrationSessionId || revision!=tm.resultConfigRevision ||
      tm.resultConfigRevision!=current.revision || !std::isfinite(measured) ||
      measured<(odo?1:10) || measured>(odo?1500:1800)) return false;
  const char* expected=odo?"odo_cal":legacyTurn?"turn_cal":left?"turn_left_cal":"turn_right_cal";
  if(std::strcmp(tm.resultType,expected)) return false;
  Config candidate=current;
  if(odo) {
    if(!(tm.calibrationPulses>0) || !std::isfinite(tm.calibrationPulses)) return false;
    candidate.pulsesPerCm=tm.calibrationPulses/measured;
    candidate.ppr=candidate.pulsesPerCm*3.14159265358979323846f*candidate.wheelMm/10;
    if(!std::isfinite(candidate.ppr) || candidate.ppr<1 || candidate.ppr>10000 || candidate.pulsesPerCm>10000) return false;
  } else {
    if(!std::isfinite(tm.target) || !std::isfinite(tm.calibrationTurnFactor) ||
        tm.target==0 || (left&&tm.target<0) || (right&&tm.target>0)) return false;
    const float factor=tm.calibrationTurnFactor*std::fabs(tm.target)/measured;
    if(!std::isfinite(factor) || factor<0.1f || factor>10) return false;
    if(legacyTurn) candidate.turnFactor=candidate.turnFactorLeft=candidate.turnFactorRight=factor;
    else if(left) candidate.turnFactorLeft=factor;
    else candidate.turnFactorRight=factor;
  }
  next=candidate; return true;
}
}
