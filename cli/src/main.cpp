// -*- mode:c++; tab-width:2; indent-tabs-mode:nil; c-basic-offset:2 -*-
/*
 *  Copyright 2010-2011 ZXing authors
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

#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <iomanip>
#include "ImageReaderSource.h"
#include <zxing/common/Counted.h>
#include <zxing/Binarizer.h>
#include <zxing/MultiFormatReader.h>
#include <zxing/Result.h>
#include <zxing/ReaderException.h>
#include <zxing/common/GlobalHistogramBinarizer.h>
#include <zxing/common/HybridBinarizer.h>
#include <exception>
#include <zxing/Exception.h>
#include <zxing/common/IllegalArgumentException.h>
#include <zxing/BinaryBitmap.h>
#include <zxing/DecodeHints.h>
#include <zxing/qrcode/QRCodeReader.h>
#include <zxing/multi/qrcode/QRCodeMultiReader.h>
#include <zxing/multi/ByQuadrantReader.h>
#include <zxing/multi/MultipleBarcodeReader.h>
#include <zxing/multi/GenericMultipleBarcodeReader.h>

using namespace std;
using namespace zxing;
using namespace zxing::multi;
using namespace zxing::qrcode;

namespace {

typedef std::chrono::steady_clock SteadyClock;
typedef SteadyClock::time_point SteadyTime;

struct PhaseTiming {
  double firstDetectMs;
  double firstDecodeMs;
  double repairedDetectMs;
  double repairedDecodeMs;
};

struct TimingPair {
  double detectMs;
  double decodeMs;
};

bool more = false;
bool test_mode = false;
bool try_harder = false;
bool search_multi = false;
bool use_hybrid = false;
bool use_global = false;
bool verbose = false;

double elapsed_ms(SteadyTime start, SteadyTime end) {
  return std::chrono::duration<double, std::milli>(end - start).count();
}

void print_phase_timing(const PhaseTiming& timing) {
  // stdout after payload would break verify scripts that read the last line.
  cerr << fixed << setprecision(3)
       << "timing first_detect_ms=" << timing.firstDetectMs
       << " first_decode_ms=" << timing.firstDecodeMs
       << " repaired_detect_ms=" << timing.repairedDetectMs
       << " repaired_decode_ms=" << timing.repairedDecodeMs
       << endl;
}

}

vector<Ref<Result> > decode(Ref<BinaryBitmap> image, DecodeHints hints) {
  Ref<Reader> reader(new MultiFormatReader);
  return vector<Ref<Result> >(1, reader->decode(image, hints));
}

vector<Ref<Result> > decode_multi(Ref<BinaryBitmap> image, DecodeHints hints) {
  MultiFormatReader delegate;
  GenericMultipleBarcodeReader reader(delegate);
  return reader.decodeMultiple(image, hints);
}

// QR timing option 1: attribute the whole QRCodeReader call to decodeMs.
// In-library normalize cost is included in decodeMs; detectMs stays 0.
vector<Ref<Result> > timed_qr_decode(
    Ref<BinaryBitmap> image, DecodeHints hints, TimingPair& timing) {
  SteadyTime t0 = SteadyClock::now();
  try {
    QRCodeReader reader;
    Ref<Result> result = reader.decode(image, hints);
    timing.decodeMs += elapsed_ms(t0, SteadyClock::now());
    return vector<Ref<Result> >(1, result);
  } catch (...) {
    timing.decodeMs += elapsed_ms(t0, SteadyClock::now());
    throw;
  }
}

vector<Ref<Result> > timed_decode(
    Ref<BinaryBitmap> image, DecodeHints hints, TimingPair& timing) {
  if (search_multi) {
    SteadyTime t0 = SteadyClock::now();
    try {
      vector<Ref<Result> > results = decode_multi(image, hints);
      timing.detectMs += elapsed_ms(t0, SteadyClock::now());
      return results;
    } catch (...) {
      timing.detectMs += elapsed_ms(t0, SteadyClock::now());
      throw;
    }
  }
  return timed_qr_decode(image, hints, timing);
}

int read_image(Ref<LuminanceSource> source, bool hybrid, string expected,
               bool hints_try_harder, TimingPair* timing) {
  vector<Ref<Result> > results;
  string cell_result;
  int res = -1;
  TimingPair localTiming = {0, 0};

  try {
    Ref<Binarizer> binarizer;
    if (hybrid) {
      binarizer = new HybridBinarizer(source);
    } else {
      binarizer = new GlobalHistogramBinarizer(source);
    }
    DecodeHints hints(DecodeHints::DEFAULT_HINT);
    hints.setTryHarder(hints_try_harder);
    Ref<BinaryBitmap> binary(new BinaryBitmap(binarizer));
    if (timing) {
      results = timed_decode(binary, hints, localTiming);
    } else if (search_multi) {
      results = decode_multi(binary, hints);
    } else {
      results = decode(binary, hints);
    }
    res = 0;
  } catch (const ReaderException& e) {
    cell_result = "zxing::ReaderException: " + string(e.what());
    res = -2;
  } catch (const zxing::IllegalArgumentException& e) {
    cell_result = "zxing::IllegalArgumentException: " + string(e.what());
    res = -3;
  } catch (const zxing::Exception& e) {
    cell_result = "zxing::Exception: " + string(e.what());
    res = -4;
  } catch (const std::exception& e) {
    cell_result = "std::exception: " + string(e.what());
    res = -5;
  }

  if (timing) {
    timing->detectMs += localTiming.detectMs;
    timing->decodeMs += localTiming.decodeMs;
  }

  if (test_mode && results.size() == 1) {
    std::string result = results[0]->getText()->getText();
    if (expected.empty()) {
      cout << "  Expected text or binary data for image missing." << endl
           << "  Detected: " << result << endl;
      res = -6;
    } else {
      if (expected.compare(result) != 0) {
        cout << "  Expected: " << expected << endl
             << "  Detected: " << result << endl;
        cell_result = "data did not match";
        res = -6;
      }
    }
  }

  if (res != 0 && (verbose || (use_global ^ use_hybrid))) {
    cout << (hybrid ? "Hybrid" : "Global")
         << " binarizer failed: " << cell_result << endl;
  } else if (!test_mode) {
    if (verbose) {
      cout << (hybrid ? "Hybrid" : "Global")
           << " binarizer succeeded: " << endl;
    }
    for (size_t i = 0; i < results.size(); i++) {
      if (more) {
        cout << "  Format: "
             << BarcodeFormat::barcodeFormatNames[results[i]->getBarcodeFormat()]
             << endl;
        for (int j = 0; j < results[i]->getResultPoints()->size(); j++) {
          cout << "  Point[" << j <<  "]: "
               << results[i]->getResultPoints()[j]->getX() << " "
               << results[i]->getResultPoints()[j]->getY() << endl;
        }
      }
      if (verbose) {
        cout << "    ";
      }
      cout << results[i]->getText()->getText() << endl;
    }
  }

  return res;
}

string read_expected(string imagefilename) {
  string textfilename = imagefilename;
  string::size_type dotpos = textfilename.rfind(".");

  textfilename.replace(dotpos + 1, textfilename.length() - dotpos - 1, "txt");
  ifstream textfile(textfilename.c_str(), ios::binary);
  textfilename.replace(dotpos + 1, textfilename.length() - dotpos - 1, "bin");
  ifstream binfile(textfilename.c_str(), ios::binary);
  ifstream *file = 0;
  if (textfile.is_open()) {
    file = &textfile;
  } else if (binfile.is_open()) {
    file = &binfile;
  } else {
    return std::string();
  }
  file->seekg(0, ios_base::end);
  size_t size = size_t(file->tellg());
  file->seekg(0, ios_base::beg);

  if (size == 0) {
    return std::string();
  }

  char* data = new char[size + 1];
  file->read(data, size);
  data[size] = '\0';
  string expected(data);
  delete[] data;

  return expected;
}

struct DecodeSummary {
  int total;
  int globalOnly;
  int hybridOnly;
  int both;
  int neither;
};

void print_usage(const char* program) {
  cout << "Usage: " << program << " [OPTION]... <IMAGE>..." << endl
       << "Read barcodes from each IMAGE file." << endl
       << endl
       << "Options:" << endl
       << "  (-h|--hybrid)             use the hybrid binarizer (default)" << endl
       << "  (-g|--global)             use the global binarizer" << endl
       << "  (-v|--verbose)            chattier results printing" << endl
       << "  --more                    display more information about the barcode" << endl
       << "  --test-mode               compare IMAGEs against text files" << endl
       << "  --try-harder              spend more time to try to find a barcode" << endl
       << "  --search-multi            search for more than one bar code" << endl
       << endl
       << "Example usage:" << endl
       << "  zxing --test-mode *.jpg" << endl
       << endl;
}

bool handle_option(const string& argument) {
  if (argument == "--verbose" || argument == "-v") {
    verbose = true;
  } else if (argument == "--hybrid" || argument == "-h") {
    use_hybrid = true;
  } else if (argument == "--global" || argument == "-g") {
    use_global = true;
  } else if (argument == "--more") {
    more = true;
  } else if (argument == "--test-mode") {
    test_mode = true;
  } else if (argument == "--try-harder") {
    try_harder = true;
  } else if (argument == "--search-multi") {
    search_multi = true;
  } else {
    return false;
  }
  return true;
}

bool is_expected_file(const string& filename) {
  if (filename.length() <= 3) {
    return false;
  }
  string extension = filename.substr(filename.length() - 3, 3);
  return extension == "txt" || extension == "bin";
}

void update_summary(DecodeSummary& summary, int globalResult, int hybridResult) {
  bool globalSuccess = globalResult == 0;
  bool hybridSuccess = hybridResult == 0;
  summary.globalOnly += globalSuccess && !hybridSuccess;
  summary.hybridOnly += hybridSuccess && !globalSuccess;
  summary.both += globalSuccess && hybridSuccess;
  summary.neither += !globalSuccess && !hybridSuccess;
  summary.total++;
}

void process_image(const string& filename, DecodeSummary& summary) {
  if (!use_global && !use_hybrid) {
    use_global = use_hybrid = true;
  }
  if (test_mode) {
    cerr << "Testing: " << filename << endl;
  }

  Ref<LuminanceSource> source;
  try {
    source = ImageReaderSource::createLoaded(filename);
  } catch (const zxing::IllegalArgumentException& e) {
    cerr << e.what() << " (ignoring)" << endl;
    return;
  }

  string expected = read_expected(filename);
  int globalResult = 1;
  int hybridResult = 1;
  PhaseTiming phase = {0, 0, 0, 0};

  auto attempt_decode = [&](Ref<LuminanceSource> attemptSource,
                            bool hintsTryHarder, bool allowHybrid,
                            bool allowGlobal) {
    TimingPair pair = {0, 0};
    if (allowHybrid && use_hybrid) {
      hybridResult = read_image(
          attemptSource, true, expected, hintsTryHarder, &pair);
    }
    if (allowGlobal && use_global &&
        (test_mode || verbose || hybridResult != 0 || !use_hybrid)) {
      globalResult = read_image(
          attemptSource, false, expected, hintsTryHarder, &pair);
    }
    phase.firstDetectMs += pair.detectMs;
    phase.firstDecodeMs += pair.decodeMs;
  };

  const bool dualBinarizer = use_hybrid && use_global;

  attempt_decode(source, try_harder, use_hybrid || !use_global,
                 !dualBinarizer || test_mode);
  cout<<"try_harder: "<<try_harder<<" use_hybrid: "<<use_hybrid<<" use_global: "<<use_global<<" dualBinarizer: "<<dualBinarizer<<" test_mode: "<<test_mode<<endl;
  cout<<"hybridResult: "<<hybridResult<<" globalResult: "<<globalResult<<endl;
  if (hybridResult != 0 && globalResult != 0 && dualBinarizer &&
      !test_mode) {
    attempt_decode(source, try_harder, false, true);
  }
  if (hybridResult != 0 && globalResult != 0 && !try_harder) {
    attempt_decode(source, true, true, true);
  }

  if (!verbose && hybridResult != 0 && globalResult != 0) {
    cout << "decoding failed" << endl;
  }
  print_phase_timing(phase);
  update_summary(summary, globalResult, hybridResult);
}

void print_summary(const DecodeSummary& summary) {
  cout << endl
       << "Summary:" << endl
       << " " << summary.total << " images tested total," << endl
       << " " << (summary.hybridOnly + summary.both) << " passed hybrid, "
       << (summary.globalOnly + summary.both) << " passed global, "
       << summary.both << " pass both, " << endl
       << " " << summary.hybridOnly << " passed only hybrid, "
       << summary.globalOnly << " passed only global, " << summary.neither
       << " pass neither." << endl;
}

int main(int argc, char** argv) {
  if (argc <= 1) {
    print_usage(argv[0]);
    return 1;
  }

  DecodeSummary summary = {0, 0, 0, 0, 0};
  for (int i = 1; i < argc; i++) {
    string argument = argv[i];
    if (!handle_option(argument) && !is_expected_file(argument)) {
      process_image(argument, summary);
    }
  }
  if (test_mode) {
    print_summary(summary);
  }
  return 0;
}
