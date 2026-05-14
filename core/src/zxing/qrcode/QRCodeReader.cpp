// -*- mode:c++; tab-width:2; indent-tabs-mode:nil; c-basic-offset:2 -*-
/*
 *  QRCodeReader.cpp
 *  zxing
 *
 *  Created by Christian Brunschen on 20/05/2008.
 *  Copyright 2008 ZXing authors All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <zxing/qrcode/QRCodeReader.h>
#include <zxing/qrcode/detector/Detector.h>
#include <zxing/common/GlobalHistogramBinarizer.h>
#include <zxing/common/HybridBinarizer.h>
#include <zxing/InvertedLuminanceSource.h>
#include <zxing/LuminanceSource.h>
#include <zxing/BinaryBitmap.h>
#include <zxing/Binarizer.h>
#include <zxing/ReaderException.h>

#include <iostream>

namespace zxing {
	namespace qrcode {

			using namespace std;

			QRCodeReader::QRCodeReader() :decoder_() {
			}

			Ref<Result> QRCodeReader::decodeInternal(Ref<BinaryBitmap> image, DecodeHints hints) {
				Detector detector(image->getBlackMatrix());
				Ref<DetectorResult> detectorResult(detector.detect(hints));
				ArrayRef< Ref<ResultPoint> > points (detectorResult->getPoints());
				Ref<DecoderResult> decoderResult(decoder_.decode(detectorResult->getBits()));
				Ref<Result> result(
					new Result(decoderResult->getText(), decoderResult->getRawBytes(), points, BarcodeFormat::QR_CODE));
				return result;
			}

			Ref<Result> QRCodeReader::decode(Ref<BinaryBitmap> image, DecodeHints hints) {
				// Primary path
				try {
					return decodeInternal(image, hints);
				} catch (ReaderException const&) {
					// Fall through to retry strategies
				}

				Ref<LuminanceSource> source = image->getLuminanceSource();

				// Retry strategies: try inversion and alternative binarizers.
				// The original binarizer type was already tried above.
				// Strategy order: inverted+sameBinarizer, normal+altBinarizer, inverted+altBinarizer
				{
					// Try inverted luminance with same binarizer type
					Ref<LuminanceSource> invSrc(source->invert());
					Ref<Binarizer> invBin(
						new GlobalHistogramBinarizer(invSrc));
					Ref<BinaryBitmap> invBmp(new BinaryBitmap(invBin));
					try {
						return decodeInternal(invBmp, hints);
					} catch (ReaderException const&) {}
				}

				{
					// Try original luminance with HybridBinarizer
					Ref<Binarizer> hybBin(new HybridBinarizer(source));
					Ref<BinaryBitmap> hybBmp(new BinaryBitmap(hybBin));
					try {
						return decodeInternal(hybBmp, hints);
					} catch (ReaderException const&) {}
				}

				{
					// Try inverted luminance with HybridBinarizer
					Ref<LuminanceSource> invSrc(source->invert());
					Ref<Binarizer> invHybBin(new HybridBinarizer(invSrc));
					Ref<BinaryBitmap> invHybBmp(new BinaryBitmap(invHybBin));
					return decodeInternal(invHybBmp, hints);
				}
			}

			QRCodeReader::~QRCodeReader() {
			}

	    Decoder& QRCodeReader::getDecoder() {
	        return decoder_;
	    }
		}
	}
