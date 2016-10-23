#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <iostream>
#include <vector>
#include <limits>
#include <cmath>
#include <chrono>
#include <ctime>
#include <memory>
#include <utility>

#include <unordered_map>
#include <unordered_set>

#include <execinfo.h>
#include <unistd.h>
#include <signal.h>

#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/program_options.hpp>

#include "cnn/training.h"
#include "cnn/cnn.h"
#include "cnn/expr.h"
#include "cnn/nodes.h"
#include "cnn/lstm.h"
#include "cnn/rnn.h"
#include "c2.h"
#include "lstm-parse.h"

using namespace cnn::expr;
using namespace cnn;
using namespace std;
namespace po = boost::program_options;

volatile bool requested_stop = false;

void InitCommandLine(int argc, char** argv, po::variables_map* conf) {
  po::options_description opts("Configuration options");
  opts.add_options()
        ("training_data,T", po::value<string>(),
         "List of transitions - training corpus")
        ("dev_data,d", po::value<string>(), "Development corpus")
        ("unk_strategy,o", po::value<unsigned>()->default_value(1),
         "Unknown word strategy: 1 = singletons become UNK with probability"
         " unk_prob")
        ("unk_prob,u", po::value<double>()->default_value(0.2),
         "Probably with which to replace singletons with UNK in training data")
        ("model,m", po::value<string>(), "Load saved model from this file")
        ("use_pos_tags,P", "make POS tags visible to parser")
        ("layers,l", po::value<unsigned>()->default_value(2),
         "number of LSTM layers")
        ("action_dim", po::value<unsigned>()->default_value(16),
         "action embedding size")
        ("input_dim", po::value<unsigned>()->default_value(32),
         "input embedding size")
        ("hidden_dim", po::value<unsigned>()->default_value(64),
         "hidden dimension")
        ("pos_dim", po::value<unsigned>()->default_value(12), "POS dimension")
        ("rel_dim", po::value<unsigned>()->default_value(10),
         "relation dimension")
        ("lstm_input_dim", po::value<unsigned>()->default_value(60),
         "LSTM input dimension")
        ("train,t", "Should training be run?")
        ("test,e", "Should the model be tested on dev data?")
        ("words,w", po::value<string>(), "Pretrained word embeddings")
        ("help,h", "Help");
  po::options_description dcmdline_options;
  dcmdline_options.add(opts);
  po::store(parse_command_line(argc, argv, dcmdline_options), *conf);
  if (conf->count("help")) {
    cerr << dcmdline_options << endl;
    exit(1);
  }
  if (conf->count("training_data") == 0) {
    cerr << "Please specify --training_data (-T): this is required to determine"
            " the vocabulary mapping, even if the parser is used in prediction"
            " mode.\n";
    exit(1);
  }
}


void signal_callback_handler(int /* signum */) {
  if (requested_stop) {
    cerr << "\nReceived SIGINT again, quitting.\n";
    _exit(1);
  }
  cerr << "\nReceived SIGINT terminating optimization early...\n";
  requested_stop = true;
}


unsigned compute_correct(const map<int, int>& ref, const map<int, int>& hyp,
                         unsigned len) {
  unsigned res = 0;
  for (unsigned i = 0; i < len; ++i) {
    auto ri = ref.find(i);
    auto hi = hyp.find(i);
    assert(ri != ref.end());
    assert(hi != hyp.end());
    if (ri->second == hi->second)
      ++res;
  }
  return res;
}


void output_conll(const vector<unsigned>& sentence, const vector<unsigned>& pos,
                  const vector<string>& sentenceUnkStrings,
                  const vector<string>& intToWords,
                  const vector<string>& intToPos,
                  const map<string, unsigned>& wordsToInt,
                  const map<int, int>& hyp, const map<int, string>& rel_hyp) {
  for (unsigned i = 0; i < (sentence.size() - 1); ++i) {
    auto index = i + 1;
    const unsigned int unk_word =
        wordsToInt.find(cpyp::ParserVocabulary::UNK)->second;
    assert(i < sentenceUnkStrings.size() &&
           ((sentence[i] == unk_word && sentenceUnkStrings[i].size() > 0) ||
            (sentence[i] != unk_word && sentenceUnkStrings[i].size() == 0 &&
             intToWords.size() > sentence[i])));
    string wit = (sentenceUnkStrings[i].size() > 0) ?
				  sentenceUnkStrings[i] : intToWords[sentence[i]];
    const string& pos_tag = intToPos[pos[i]];
    assert(hyp.find(i) != hyp.end());
    auto hyp_head = hyp.find(i)->second + 1;
    if (hyp_head == (int) sentence.size())
      hyp_head = 0;
    auto hyp_rel_it = rel_hyp.find(i);
    assert(hyp_rel_it != rel_hyp.end());
    auto hyp_rel = hyp_rel_it->second;
    size_t first_char_in_rel = hyp_rel.find('(') + 1;
    size_t last_char_in_rel = hyp_rel.rfind(')') - 1;
    hyp_rel = hyp_rel.substr(first_char_in_rel,
                             last_char_in_rel - first_char_in_rel + 1);
    cout << index << '\t'       // 1. ID
         << wit << '\t'         // 2. FORM
         << "_" << '\t'         // 3. LEMMA
         << "_" << '\t'         // 4. CPOSTAG
         << pos_tag << '\t'     // 5. POSTAG
         << "_" << '\t'         // 6. FEATS
         << hyp_head << '\t'    // 7. HEAD
         << hyp_rel << '\t'     // 8. DEPREL
         << "_" << '\t'         // 9. PHEAD
         << "_" << endl;        // 10. PDEPREL
  }
  cout << endl;
}


void do_train(ParserBuilder* parser, const unsigned unk_strategy,
              const set<unsigned>& singletons, const double unk_prob,
              const set<unsigned>& training_vocab, const string& fname) {
  bool softlinkCreated = false;
  int best_correct_heads = 0;
  unsigned status_every_i_iterations = 100;
  signal(SIGINT, signal_callback_handler);
  SimpleSGDTrainer sgd(&parser->model);
  //MomentumSGDTrainer sgd(model);
  sgd.eta_decay = 0.08;
  //sgd.eta_decay = 0.05;
  const cpyp::Corpus& corpus = parser->corpus;
  vector<unsigned> order(corpus.nsentences);
  for (unsigned i = 0; i < corpus.nsentences; ++i)
    order[i] = i;
  double tot_seen = 0;
  status_every_i_iterations = min(status_every_i_iterations, corpus.nsentences);
  unsigned si = corpus.nsentences;
  cerr << "NUMBER OF TRAINING SENTENCES: " << corpus.nsentences << endl;
  unsigned trs = 0;
  double right = 0;
  double llh = 0;
  bool first = true;
  int iter = -1;
  time_t time_start = std::chrono::system_clock::to_time_t(
      std::chrono::system_clock::now());
  cerr << "TRAINING STARTED AT: " << put_time(localtime(&time_start), "%c %Z")
       << endl;
  while (!requested_stop) {
    ++iter;
    for (unsigned sii = 0; sii < status_every_i_iterations; ++sii) {
      if (si == corpus.nsentences) {
        si = 0;
        if (first) {
          first = false;
        } else {
          sgd.update_epoch();
        }
        cerr << "**SHUFFLE\n";
        random_shuffle(order.begin(), order.end());
      }
      tot_seen += 1;
      const vector<unsigned>& sentence =
          corpus.sentences.find(order[si])->second;
      vector<unsigned> tsentence = sentence;
      if (unk_strategy == 1) {
        for (auto& w : tsentence) {
          if (singletons.count(w) && cnn::rand01() < unk_prob) {
            w = parser->kUNK;
          }
        }
      }
      const vector<unsigned>& sentencePos =
          corpus.sentencesPos.find(order[si])->second;
      const vector<unsigned>& actions =
          corpus.correct_act_sent.find(order[si])->second;
      ComputationGraph hg;
      parser->LogProbParser(&hg, sentence, tsentence, sentencePos, actions,
                            corpus.vocab->actions, corpus.vocab->intToWords,
                            &right);
      double lp = as_scalar(hg.incremental_forward());
      if (lp < 0) {
        cerr << "Log prob < 0 on sentence " << order[si] << ": lp=" << lp
            << endl;
        assert(lp >= 0.0);
      }
      hg.backward();
      sgd.update(1.0);
      llh += lp;
      ++si;
      trs += actions.size();
    }
    sgd.status();
    time_t time_now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    cerr << "update #" << iter << " (epoch " << (tot_seen / corpus.nsentences)
         << " |time=" << put_time(localtime(&time_now), "%c %Z") << ")\tllh: "
         << llh << " ppl: " << exp(llh / trs) << " err: " << (trs - right) / trs
         << endl;
    llh = trs = right = 0;
    static int logc = 0;
    ++logc;
    if (logc % 25 == 1) {
      // report on dev set
      unsigned dev_size = corpus.nsentencesDev;
      // dev_size = 100;
      double llh = 0;
      double trs = 0;
      double right = 0;
      double correct_heads = 0;
      double total_heads = 0;
      auto t_start = std::chrono::high_resolution_clock::now();
      for (unsigned sii = 0; sii < dev_size; ++sii) {
        const vector<unsigned>& sentence =
            corpus.sentencesDev.find(sii)->second;
        const vector<unsigned>& sentencePos =
            corpus.sentencesPosDev.find(sii)->second;
        const vector<unsigned>& actions =
            corpus.correct_act_sentDev.find(sii)->second;
        vector<unsigned> tsentence = sentence;
        for (auto& w : tsentence)
          if (training_vocab.count(w) == 0)
            w = parser->kUNK;
        ComputationGraph hg;
        vector<unsigned> pred = parser->LogProbParser(
            &hg, sentence, tsentence, sentencePos, vector<unsigned>(),
            corpus.vocab->actions, corpus.vocab->intToWords, &right);
        double lp = 0;
        llh -= lp;
        trs += actions.size();
        map<int, int> ref = parser->ComputeHeads(sentence.size(), actions,
                                                 corpus.vocab->actions);
        map<int, int> hyp = parser->ComputeHeads(sentence.size(), pred,
                                                 corpus.vocab->actions);
        correct_heads += compute_correct(ref, hyp, sentence.size() - 1);
        total_heads += sentence.size() - 1;
      }
      auto t_end = std::chrono::high_resolution_clock::now();
      cerr << "  **dev (iter=" << iter << " epoch="
           << (tot_seen / corpus.nsentences) << ")\tllh=" << llh << " ppl: "
           << exp(llh / trs) << " err: " << (trs - right) / trs << " uas: "
           << (correct_heads / total_heads) << "\t[" << dev_size << " sents in "
           << std::chrono::duration<double, std::milli>(t_end - t_start).count()
           << " ms]" << endl;
      if (correct_heads > best_correct_heads) {
        best_correct_heads = correct_heads;
        ofstream out(fname);
        boost::archive::text_oarchive oa(out);
        oa << parser->options;
        oa << parser->model;
        // Create a soft link to the most recent model in order to make it
        // easier to refer to it in a shell script.
        if (!softlinkCreated) {
          string softlink = "latest_model.params";
          if (system((string("rm -f ") + softlink).c_str()) == 0
              && system((string("ln -s ") + fname + " " + softlink).c_str())
                  == 0) {
            cerr << "Created " << softlink << " as a soft link to " << fname
                 << " for convenience." << endl;
          }
          softlinkCreated = true;
        }
      }
    }
  }
}


void do_test(ParserBuilder* parser, const set<unsigned>& training_vocab) {
  // do test evaluation
  double llh = 0;
  double trs = 0;
  double right = 0;
  double correct_heads = 0;
  double total_heads = 0;
  auto t_start = std::chrono::high_resolution_clock::now();
  const cpyp::Corpus& corpus = parser->corpus;
  unsigned corpus_size = corpus.nsentencesDev;
  for (unsigned sii = 0; sii < corpus_size; ++sii) {
    const vector<unsigned>& sentence = corpus.sentencesDev.find(sii)->second;
    const vector<unsigned>& sentencePos =
        corpus.sentencesPosDev.find(sii)->second;
    const vector<string>& sentenceUnkStr =
        corpus.sentencesStrDev.find(sii)->second;
    const vector<unsigned>& actions =
        corpus.correct_act_sentDev.find(sii)->second;
    vector<unsigned> tsentence = sentence;
    for (auto& w : tsentence)
      if (training_vocab.count(w) == 0)
        w = parser->kUNK;
    ComputationGraph cg;
    double lp = 0;
    vector<unsigned> pred;
    pred = parser->LogProbParser(&cg, sentence, tsentence, sentencePos,
                                 vector<unsigned>(), corpus.vocab->actions,
                                 corpus.vocab->intToWords, &right);
    llh -= lp;
    trs += actions.size();
    map<int, string> rel_ref;
    map<int, string> rel_hyp;
    map<int, int> ref = parser->ComputeHeads(sentence.size(), actions,
                                             corpus.vocab->actions, &rel_ref);
    map<int, int> hyp = parser->ComputeHeads(sentence.size(), pred,
                                             corpus.vocab->actions, &rel_hyp);
    output_conll(sentence, sentencePos, sentenceUnkStr,
                 corpus.vocab->intToWords, corpus.vocab->intToPos,
                 corpus.vocab->wordsToInt, hyp, rel_hyp);
    correct_heads += compute_correct(ref, hyp, sentence.size() - 1);
    total_heads += sentence.size() - 1;
  }
  auto t_end = std::chrono::high_resolution_clock::now();
  cerr << "TEST llh=" << llh << " ppl: " << exp(llh / trs) << " err: "
       << (trs - right) / trs << " uas: " << (correct_heads / total_heads)
       << "\t[" << corpus_size << " sents in "
       << std::chrono::duration<double, std::milli>(t_end - t_start).count()
       << " ms]" << endl;
}


int main(int argc, char** argv) {
  cnn::Initialize(argc, argv);

  cerr << "COMMAND:";
  for (unsigned i = 0; i < static_cast<unsigned>(argc); ++i)
    cerr << ' ' << argv[i];
  cerr << endl;

  po::variables_map conf;
  InitCommandLine(argc, argv, &conf);

  bool load_model = conf.count("model");
  unique_ptr<boost::archive::text_iarchive> archive_ptr;
  unique_ptr<ifstream> model_stream_ptr;

  ParserOptions options {
    conf.count("use_pos_tags"),
    conf["layers"].as<unsigned>(),
    conf["input_dim"].as<unsigned>(),
    conf["hidden_dim"].as<unsigned>(),
    conf["action_dim"].as<unsigned>(),
    conf["lstm_input_dim"].as<unsigned>(),
    conf["pos_dim"].as<unsigned>(),
    conf["rel_dim"].as<unsigned>()
  };

  // Training options and operation options
  const unsigned unk_strategy = conf["unk_strategy"].as<unsigned>();
  const double unk_prob = conf["unk_prob"].as<double>();
  const bool train = conf.count("train");
  const bool test = conf.count("test");

  cerr << "Unknown word strategy: ";
  if (unk_strategy == 1) {
    cerr << "STOCHASTIC REPLACEMENT\n";
  } else {
    abort();
  }
  assert(unk_prob >= 0. && unk_prob <= 1.);

  if (load_model) {
    const string& model_path = conf["model"].as<string>();
    cerr << "Loading model from " << model_path << endl;
    model_stream_ptr.reset(new ifstream(model_path.c_str()));
    archive_ptr.reset(new boost::archive::text_iarchive(*model_stream_ptr));

    ParserOptions stored_options{};
    *archive_ptr >> stored_options;
    if (stored_options != options) {
      cerr << "WARNING: overriding command line network options with stored"
              " options" << endl;
      options = stored_options;
    }
  }

  ParserBuilder parser(conf["training_data"].as<string>(),
                       conf["words"].as<string>(), options);
  if (load_model) {
    *archive_ptr >> parser.model;
  }

  set<unsigned> training_vocab; // words available in the training corpus
  set<unsigned> singletons;
  {  // compute the singletons in the parser's training data
    map<unsigned, unsigned> counts;
    for (auto sent : parser.corpus.sentences)
      for (auto word : sent.second) {
        training_vocab.insert(word);
        counts[word]++;
      }
    if (train) {
      for (auto wc : counts)
        if (wc.second == 1)
          singletons.insert(wc.first);
    }
  }

  cerr << "Total number of words: " << parser.corpus.vocab->CountWords()
       << endl;

  // OOV words will be replaced by UNK tokens
  parser.corpus.load_correct_actionsDev(conf["dev_data"].as<string>());
  if (train) {
    ostringstream os;
    os << "parser_" << (options.use_pos ? "pos" : "nopos")
       << '_' << options.layers
       << '_' << options.input_dim
       << '_' << options.hidden_dim
       << '_' << options.action_dim
       << '_' << options.lstm_input_dim
       << '_' << options.pos_dim
       << '_' << options.rel_dim
       << "-pid" << getpid() << ".params";
    const string fname = os.str();
    cerr << "Writing parameters to file: " << fname << endl;
    do_train(&parser, unk_strategy, singletons, unk_prob, training_vocab,
             fname);
  }
  if (test) { // do test evaluation
    do_test(&parser, training_vocab);
  }

  /*
  for (unsigned i = 0; i < corpus.actions.size(); ++i) {
    cerr << corpus.actions[i] << '\t' << parser.p_r->values[i].transpose()
         << endl;
    cerr << corpus.actions[i] << '\t'
         << parser.p_p2a->values.col(i).transpose() << endl;
  }
  //*/
}
