/*
 * Copyright (C) 2007, 2008, 2012 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef CSSTimingFunctionValue_h
#define CSSTimingFunctionValue_h

#include "core/css/CSSValue.h"
#include "platform/RuntimeEnabledFeatures.h"
#include "platform/animation/TimingFunction.h"
#include "platform/wtf/RefPtr.h"

namespace blink {

class CSSCubicBezierTimingFunctionValue : public CSSValue {
 public:
  static CSSCubicBezierTimingFunctionValue* Create(double x1,
                                                   double y1,
                                                   double x2,
                                                   double y2) {
    return new CSSCubicBezierTimingFunctionValue(x1, y1, x2, y2);
  }

  String CustomCSSText() const;

  double X1() const { return x1_; }
  double Y1() const { return y1_; }
  double X2() const { return x2_; }
  double Y2() const { return y2_; }

  bool Equals(const CSSCubicBezierTimingFunctionValue&) const;

  DEFINE_INLINE_TRACE_AFTER_DISPATCH() {
    CSSValue::TraceAfterDispatch(visitor);
  }

 private:
  CSSCubicBezierTimingFunctionValue(double x1, double y1, double x2, double y2)
      : CSSValue(kCubicBezierTimingFunctionClass),
        x1_(x1),
        y1_(y1),
        x2_(x2),
        y2_(y2) {}

  double x1_;
  double y1_;
  double x2_;
  double y2_;
};

DEFINE_CSS_VALUE_TYPE_CASTS(CSSCubicBezierTimingFunctionValue,
                            IsCubicBezierTimingFunctionValue());

class CSSStepsTimingFunctionValue : public CSSValue {
 public:
  static CSSStepsTimingFunctionValue* Create(
      int steps,
      StepsTimingFunction::StepPosition step_position) {
    return new CSSStepsTimingFunctionValue(steps, step_position);
  }

  int NumberOfSteps() const { return steps_; }
  StepsTimingFunction::StepPosition GetStepPosition() const {
    return step_position_;
  }

  String CustomCSSText() const;

  bool Equals(const CSSStepsTimingFunctionValue&) const;

  DEFINE_INLINE_TRACE_AFTER_DISPATCH() {
    CSSValue::TraceAfterDispatch(visitor);
  }

 private:
  CSSStepsTimingFunctionValue(int steps,
                              StepsTimingFunction::StepPosition step_position)
      : CSSValue(kStepsTimingFunctionClass),
        steps_(steps),
        step_position_(step_position) {}

  int steps_;
  StepsTimingFunction::StepPosition step_position_;
};

DEFINE_CSS_VALUE_TYPE_CASTS(CSSStepsTimingFunctionValue,
                            IsStepsTimingFunctionValue());

class CSSFramesTimingFunctionValue : public CSSValue {
 public:
  static CSSFramesTimingFunctionValue* Create(int frames) {
    return new CSSFramesTimingFunctionValue(frames);
  }

  int NumberOfFrames() const { return frames_; }

  String CustomCSSText() const;

  bool Equals(const CSSFramesTimingFunctionValue&) const;

  DEFINE_INLINE_TRACE_AFTER_DISPATCH() {
    CSSValue::TraceAfterDispatch(visitor);
  }

 private:
  CSSFramesTimingFunctionValue(int frames)
      : CSSValue(kFramesTimingFunctionClass), frames_(frames) {
    DCHECK(RuntimeEnabledFeatures::FramesTimingFunctionEnabled());
  }

  int frames_;
};

DEFINE_CSS_VALUE_TYPE_CASTS(CSSFramesTimingFunctionValue,
                            IsFramesTimingFunctionValue());

}  // namespace blink

#endif
