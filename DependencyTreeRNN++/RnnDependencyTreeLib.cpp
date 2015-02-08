// Copyright (c) 2014-2015 Piotr Mirowski. All rights reserved.
//                         piotr.mirowski@computer.org
//
// Based on code by Geoffrey Zweig and Tomas Mikolov
// for the Recurrent Neural Networks Language Model (RNNLM) toolbox
//
// Recurrent neural network based statistical language modeling toolkitsize
// Version 0.3f
// (c) 2010-2012 Tomas Mikolov (tmikolov@gmail.com)
// Extensions from 0.3e to 0.3f version done at Microsoft Research
//
// This code implements the following paper:
//   Tomas Mikolov and Geoffrey Zweig
//   "Context Dependent Recurrent Neural Network Language Model"
//   Microsoft Research Technical Report MSR-TR-2012-92 July 27th, 2012
//   IEEE Conference on Spoken Language Technologies
//   http://research.microsoft.com/apps/pubs/default.aspx?id=176926

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <map>
#include <iostream>
#include <sstream>
#include <assert.h>
#include "ReadJson.h"
#include "RnnState.h"
#include "CorpusUnrollsReader.h"
#include "RnnDependencyTreeLib.h"

// Include BLAS
#ifdef USE_BLAS
extern "C" {
#include <cblas.h>
}
#endif


/// <summary>
/// Return the index of a label in the vocabulary, or -1 if OOV.
/// </summary>
int RnnTreeLM::SearchLabelInVocabulary(const std::string& label) const {
  auto i = m_mapLabel2Index.find(label);
  if (i == m_mapLabel2Index.end()) {
    return -1;
  } else {
    return (i->second);
  }
}


/// <summary>
/// Before learning the RNN model, we need to learn the vocabulary
/// from the corpus. Note that the word classes may have been initialized
/// beforehand using ReadClasses. Computes the unigram distribution
/// of words from a training file, assuming that the existing vocabulary
/// is empty.
/// </summary>
bool RnnTreeLM::LearnVocabularyFromTrainFile() {
  // Read the vocabulary from all the files
  // OOV <unk> and EOS </s> tokens are added automatically.
  // Also count the number of words in all the books.
  m_numTrainWords =
  m_corpusVocabulary.ReadVocabulary(m_typeOfDepLabels == 1);
  
  // Filter the vocabulary based on frequency (>= 3)
  // and sort it based on frequency
  m_corpusTrain.FilterSortVocabulary(m_corpusVocabulary);
  
  // We (re)initialize the vocabulary vector,
  // the word -> index map and the index -> word map,
  // but not the word -> class map which may have been loaded by ReadClasses.
  // Note that the map word -> index will be rebuilt after sorting the vocabulary.
  m_vocabularyStorage.clear();
  m_mapWord2Index.clear();
  m_mapIndex2Word.clear();
  
  // Reinitialize the label -> index map
  m_mapLabel2Index.clear();
  
  // We cannot use a class file... (classes need to be frequency-based)
  if (m_usesClassFile) {
    cerr << "Class files not implemented\n";
    return false;
  }
  
  // Print the vocabulary size and store the file size in words
  printf("Vocab size (before pruning): %d\n",
         m_corpusVocabulary.NumWords());
  printf("Vocab size (after pruning): %d\n",
         m_corpusTrain.NumWords());
  printf("Label vocab size: %d\n",
         m_corpusTrain.NumLabels());
  
  // The first word needs to be end-of-sentence? TBD...
  AddWordToVocabulary("</s>");
  
  // Copy the words currently in the corpus
  // and insert them into the vocabulary of the RNN
  // and to the maps: word <-> index
  for (int k = 0; k < m_corpusTrain.NumWords(); k++) {
    // Get the word
    string word = m_corpusTrain.vocabularyReverse[k];
    // Lookup it up in the vocabulary
    int index = SearchWordInVocabulary(word);
    if (index == -1) {
      // Add it to the vocabulary if required
      index = AddWordToVocabulary(word);
    }
    // Store the count of words in the vocabulary
    double count = m_corpusTrain.wordCountsDiscounted[k];
    m_vocabularyStorage[index].cn = (int)round(count);
    // Add the word to the hash table word -> index
    m_mapWord2Index[word] = index;
    // Add the word to the hash table index -> word
    m_mapIndex2Word[index] = word;
  }
  
  // Copy the labels currently in the corpus
  int index = 0;
  for (int k = 0; k < m_corpusTrain.NumLabels(); k++) {
    // Get the word
    string label = m_corpusTrain.labelsReverse[k];
    // Lookup it up in the vocabulary of labels
    if (SearchLabelInVocabulary(label) == -1) {
      // Add it to the vocabulary if required
      m_mapLabel2Index[label] = index;
      index++;
    }
  }
  
  // Copy the vocabulary to the other corpus
  m_corpusValidTest.CopyVocabulary(m_corpusTrain);
  
  printf("Vocab size: %d\n", GetVocabularySize());
  printf("Label vocab size: %d\n", GetLabelSize());
  printf("Words in train file: %ld\n", m_numTrainWords);
  return true;
}


/// <summary>
/// Reset the vector of feature labels
/// </summary>
void RnnTreeLM::ResetFeatureLabelVector(RnnState &state) const {
  state.FeatureLayer.assign(GetFeatureSize(), 0.0);
}


/// <summary>
/// Update the vector of feature labels
/// </summary>
void RnnTreeLM::UpdateFeatureLabelVector(int label, RnnState &state) const {
  // Time-decay the previous labels using weight gamma
  int sizeFeatures = GetFeatureSize();
  for (int a = 0; a < sizeFeatures; a++) {
    state.FeatureLayer[a] *= m_featureGammaCoeff;
  }
  // Find the current label and set it to 1
  if ((label >= 0) && (label < sizeFeatures)) {
    state.FeatureLayer[label] = 1.0;
  }
}


/// <summary>
/// Train a Recurrent Neural Network model on a test file
/// using the JSON trees of dependency parse
/// </summary>
bool RnnTreeLM::TrainRnnModel() {
  // We do not use an external file with feature vectors;
  // feature labels are provided in the parse tree itself
  int sizeFeature = GetFeatureSize();
  
  // Reset the log-likelihood of the last iteration to ginourmous value
  double lastValidLogProbability = -1E37;
  // Word counter, saved at the end of last training session
  m_wordCounter = m_currentPosTrainFile;
  // Keep track of the initial learning rate
  m_initialLearningRate = m_learningRate;
  
  // Sanity check
  if (m_numOutputClasses > GetVocabularySize()) {
    cout << "WARNING: number of classes exceeds vocabulary size\n";
  }
  
  // Load the labels
  LoadCorrectSentenceLabels("/Users/piotr/Documents/Projets/Microsoft/Data/GutenbergHolmes/valid.labels");
  
  // Log file
  string logFilename = "/Users/piotr/Documents/Projets/Microsoft/Data/Log.txt";
  ofstream logFile(logFilename);
  cout << "Starting training tree-dependent LM using list of books "
  << m_trainFile << "...\n";
  
  bool loopEpochs = true;
  while (loopEpochs) {
    // Reset the log-likelihood of the current iteration
    double trainLogProbability = 0.0;
    // Unique word counter (count only once each word token in a sentence)
    int uniqueWordCounter = 0;
    
    // Print current epoch and learning rate
    cout << "Iter: " << m_iteration << " Alpha: " << m_learningRate << "\n";
    
    // Reset everything, including word history
    ResetAllRnnActivations(m_state);
    
    double timeGet = 0;
    double timeUpdateLabel = 0;
    double timeFProp = 0;
    double timePPX = 0;
    double timeShiftBPTT = 0;
    double timeBackProp = 0;
    double timeConnectRNN = 0;
    clock_t t0 = 0;
    clock_t t1 = 0;
    
    // Loop over the books
    clock_t start = clock();
    for (int idxBook = 0; idxBook < m_corpusTrain.NumBooks(); idxBook++) {
      // Read the next book (training file)
      m_corpusTrain.NextBook();
      m_corpusTrain.ReadBook(m_typeOfDepLabels == 1);
      BookUnrolls book = m_corpusTrain.m_currentBook;
      
      // Loop over the sentences in that book
      book.ResetSentence();
      for (int idxSentence = 0; idxSentence < book.NumSentences(); idxSentence++) {
        // Initialize a map of log-likelihoods for each token
        unordered_map<int, double> logProbSentence;
        
        // Loop over the unrolls in each sentence
        book.ResetUnroll();
        int numUnrolls = book.NumUnrolls(idxSentence);
        for (int idxUnroll = 0; idxUnroll < numUnrolls; idxUnroll++) {
          // Reset the state of the neural net before each unroll
          ResetHiddenRnnStateAndWordHistory(m_state);
          // Reset the dependency label features
          // at the beginning of each unroll
          ResetFeatureLabelVector(m_state);
          
          // At the beginning of an unroll,
          // the last word is reset to </s> (end of sentence)
          // and the last label is reset to 0 (root)
          int lastWord = 0;
          int lastLabel = 0;
          
          // Loop over the tokens in the sentence unroll
          bool ok = true;
          while (ok) {
            t0 = clock();
            
            // Get the current word, discount and label
            int tokenNumber = book.CurrentTokenNumberInSentence();
            int word = book.CurrentTokenWord();
            double discount = book.CurrentTokenDiscount();
            int label = book.CurrentTokenLabel();
            t1 = clock();
            timeGet += t1 - t0;
            t0 = t1;
//            printf("%s(%s) -> %s(%s)\n",
//                   m_corpusTrain.vocabularyReverse[lastWord].c_str(),
//                   m_corpusTrain.labelsReverse[lastLabel].c_str(),
//                   m_corpusTrain.vocabularyReverse[word].c_str(),
//                   m_corpusTrain.labelsReverse[label].c_str());
            
            // Update the feature matrix with the last dependency label
            if (m_typeOfDepLabels == 2) {
              UpdateFeatureLabelVector(lastLabel, m_state);
            }
            t1 = clock();
            timeUpdateLabel += t1 - t0;
            t0 = t1;
            
            // Run one step of the RNN to predict word
            // from lastWord, lastLabel and the last hidden state
            ForwardPropagateOneStep(lastWord, word, m_state);
            t1 = clock();
            timeFProp += t1 - t0;
            t0 = t1;
            
            // For perplexity, we do not count OOV words...
            if (word >= 0) {
              // Compute the log-probability of the current word
              int outputNodeClass =
              m_vocabularyStorage[word].classIndex + GetVocabularySize();
              double condProbaClass =
              m_state.OutputLayer[outputNodeClass];
              double condProbaWordGivenClass =
              m_state.OutputLayer[word];
              double logProbabilityWord =
              log10(condProbaClass * condProbaWordGivenClass);
              
              // Did we see already that word token (at that position)
              // in the sentence?
              if (logProbSentence.find(tokenNumber) == logProbSentence.end()) {
                // No: store the log-likelihood of that word
                logProbSentence[tokenNumber] = logProbabilityWord;
                // Contribute the log-likelihood to the sentence and corpus
                trainLogProbability += logProbabilityWord;
                uniqueWordCounter++;
              }
              m_wordCounter++;
            }
            
            // Safety check (that log-likelihood does not diverge)
            if (trainLogProbability != trainLogProbability) {
              // || (isinf(trainLogProbability)
              cout << "\nNumerical error infinite log-likelihood\n";
              return false;
            }
            t1 = clock();
            timePPX += t1 - t0;
            t0 = t1;
            
            // Shift memory needed for BPTT to next time step
            if (m_numBpttSteps > 0) {
              // shift memory needed for bptt to next time step
              for (int a = m_numBpttSteps+m_bpttBlockSize-1; a > 0; a--) {
                m_bpttVectors.History[a] = m_bpttVectors.History[a-1];
              }
              m_bpttVectors.History[0] = lastWord;
              
              int sizeHidden = GetHiddenSize();
              for (int a = m_numBpttSteps+m_bpttBlockSize-1; a > 0; a--) {
                for (int b = 0; b < sizeHidden; b++) {
                  m_bpttVectors.HiddenLayer[a * sizeHidden+b] = m_bpttVectors.HiddenLayer[(a-1) * sizeHidden+b];
                  m_bpttVectors.HiddenGradient[a * sizeHidden+b] = m_bpttVectors.HiddenGradient[(a-1) * sizeHidden+b];
                }
              }
              
              for (int a = m_numBpttSteps+m_bpttBlockSize-1; a>0; a--) {
                for (int b = 0; b < sizeFeature; b++) {
                  m_bpttVectors.FeatureLayer[a * sizeFeature+b] = m_bpttVectors.FeatureLayer[(a-1) * sizeFeature+b];
                }
              }
            }
            t1 = clock();
            timeShiftBPTT += t1 - t0;
            t0 = t1;
            
            // Discount the learning rate to handle
            // multiple occurrences of the same word
            // in the dependency parse tree
            double alphaBackup = m_learningRate;
            m_learningRate *= discount;
            
            // Back-propagate the error and run one step of
            // stochastic gradient descent (SGD) using optional
            // back-propagation through time (BPTT)
            BackPropagateErrorsThenOneStepGradientDescent(lastWord, word);
            t1 = clock();
            timeBackProp += t1 - t0;
            t0 = t1;
            
            // Undiscount the learning rate
            m_learningRate = alphaBackup;
            
            // Store the current state s(t) at the end of the input layer
            // vector so that it can be used as s(t-1) at the next step
            ForwardPropagateRecurrentConnectionOnly(m_state);
            
            // Rotate the word history by one
            ForwardPropagateWordHistory(m_state, lastWord, word);
            // Update the last label
            lastLabel = label;
            t1 = clock();
            timeConnectRNN += t1 - t0;
            t0 = t1;
            
            // Go to the next word
            ok = (book.NextTokenInUnroll() >= 0);
          } // Loop over tokens in the unroll of a sentence
          book.NextUnrollInSentence();
        } // Loop over unrolls of a sentence
        
        // Verbose
        if ((idxSentence % 1000) == 0) {
          clock_t now = clock();
          double entropy =
          -trainLogProbability/log10((double)2) / uniqueWordCounter;
          double perplexity =
          ExponentiateBase10(-trainLogProbability / (double)uniqueWordCounter);
          ostringstream buf;
          buf << "Iter," << m_iteration
          << ",Book," << idxBook
          << ",Alpha," << m_learningRate
          << ",TRAINentropy," << entropy
          << ",TRAINppx," << perplexity
          << ",fraction," << 100 * m_wordCounter/((double)m_numTrainWords)
          << ",words/sec," << 1000000 * (m_wordCounter/((double)(now-start)));
          buf << "\n";
          logFile << buf.str();
          cout << buf.str();
          cout << "TimeSpent," << idxSentence << ","
          << timeGet << "," << timeUpdateLabel << ","
          << timeFProp << "," << timePPX << ","
          << timeShiftBPTT << "," << timeBackProp << ","
          << timeConnectRNN << "\n";
        }
        
        book.NextSentence();
      } // loop over sentences for one epoch
    } // loop over books for one epoch
    
    // Verbose the iteration
    double trainEntropy = -trainLogProbability/log10((double)2) / uniqueWordCounter;
    double trainPerplexity =
    ExponentiateBase10(-trainLogProbability / (double)uniqueWordCounter);
    clock_t now = clock();
    ostringstream buf;
    buf << "Iter," << m_iteration
    << ",Alpha," << m_learningRate
    << ",Book,ALL"
    << ",TRAINentropy," << trainEntropy
    << ",TRAINppx," << trainPerplexity
    << ",fraction,100"
    << ",words/sec," << 1000000 * (m_wordCounter/((double)(now-start)));
    buf << "\n";
    logFile << buf.str();
    cout << buf.str();
    
    // Validation
    int validWordCounter = 0;
    vector<double> sentenceScores;
    double validLogProbability =
    TestRnnModel(m_validationFile,
                 m_featureValidationFile,
                 validWordCounter,
                 sentenceScores);
    double validPerplexity =
    (validWordCounter == 0) ? 0 :
    ExponentiateBase10(-validLogProbability / (double)validWordCounter);
    double validEntropy =
    (validWordCounter == 0) ? 0 :
    -validLogProbability / log10((double)2) / validWordCounter;
    
    // Compute the validation accuracy
    double validAccuracy =
    AccuracyNBestList(sentenceScores, m_correctSentenceLabels);
    cout << "Accuracy " << validAccuracy * 100 << "% on "
    << sentenceScores.size() << " sentences\n";
    
    ostringstream buf2;
    buf2 << "Iter," << m_iteration
    << ",Alpha," << m_learningRate
    << ",VALIDaccuracy," << validAccuracy
    << ",VALIDentropy," << validEntropy
    << ",VALIDppx," << validPerplexity
    << ",fraction,100"
    << ",words/sec," << 1000000 * (m_wordCounter/((double)(now-start)));
    buf2 << "\n";
    logFile << buf2.str();
    cout << buf2.str();
    
    // Reset the position in the training file
    m_wordCounter = 0;
    m_currentPosTrainFile = 0;
    trainLogProbability = 0;
    
    if (validLogProbability < lastValidLogProbability) {
      // Restore the weights and the state from the backup
      m_weights = m_weightsBackup;
      m_state = m_stateBackup;
      cout << "Restored the weights from previous iteration\n";
    } else {
      // Backup the weights and the state
      m_weightsBackup = m_weights;
      m_stateBackup = m_state;
      cout << "Save this model\n";
    }
    
    // Shall we start reducing the learning rate?
    if (validLogProbability * m_minLogProbaImprovement < lastValidLogProbability) {
      if (!m_doStartReducingLearningRate) {
        m_doStartReducingLearningRate = true;
      } else {
        SaveRnnModelToFile();
        // Let's also save the word embeddings
        SaveWordEmbeddings(m_rnnModelFile + ".word_embeddings.txt");
        loopEpochs = false;
        break;
      }
    }
    
    if (loopEpochs) {
      if (m_doStartReducingLearningRate) {
        m_learningRate /= 2;
      }
      lastValidLogProbability = validLogProbability;
      validLogProbability = 0;
      m_iteration++;
      SaveRnnModelToFile();
      // Let's also save the word embeddings
      SaveWordEmbeddings(m_rnnModelFile + ".word_embeddings.txt");
      printf("Saved the model\n");
    }
  }
  
  return true;
}


/// <summary>
/// Test a Recurrent Neural Network model on a test file
/// </summary>
double RnnTreeLM::TestRnnModel(const string &testFile,
                               const string &featureFile,
                               int &uniqueWordCounter,
                               vector<double> &sentenceScores) {
  cout << "RnnTreeLM::testNet()\n";
  
  // We do not use an external file with feature vectors;
  // feature labels are provided in the parse tree itself
  
  // This function does what ResetHiddenRnnStateAndWordHistory does
  // and also resets the features, inputs, outputs and compression layer
  ResetAllRnnActivations(m_state);
  
  // Reset the log-likelihood
  double testLogProbability = 0.0;
  // Reset the unique word token counter
  uniqueWordCounter = 0;
  int numUnk = 0;
  
  // Since we just set s(1)=0, this will set the state s(t-1) to 0 as well...
  ForwardPropagateRecurrentConnectionOnly(m_state);
  
  // Loop over the books
  for (int idxBook = 0; idxBook < m_corpusValidTest.NumBooks(); idxBook++) {
    // Read the next book
    m_corpusValidTest.NextBook();
    m_corpusValidTest.ReadBook(m_typeOfDepLabels == 1);
    BookUnrolls book = m_corpusValidTest.m_currentBook;
    
    // Loop over the sentences in the book
    book.ResetSentence();
    for (int idxSentence = 0; idxSentence < book.NumSentences(); idxSentence++) {
      // Initialize a map of log-likelihoods for each token
      unordered_map<int, double> logProbSentence;
      // Reset the log-likelihood of the sentence
      double sentenceLogProbability = 0.0;
      
      // Loop over the unrolls in each sentence
      book.ResetUnroll();
      int numUnrolls = book.NumUnrolls(idxSentence);
      for (int idxUnroll = 0; idxUnroll < numUnrolls; idxUnroll++)
      {
        // Reset the state of the neural net before each unroll
        ResetHiddenRnnStateAndWordHistory(m_state);
        // Reset the dependency label features
        // at the beginning of each unroll
        ResetFeatureLabelVector(m_state);
        
        // At the beginning of an unroll,
        // the last word is reset to </s> (end of sentence)
        // and the last label is reset to 0 (root)
        int lastWord = 0;
        int lastLabel = 0;
        
        // Loop over the tokens in the sentence unroll
        bool ok = true;
        while (ok) {
          // Get the current word, discount and label
          int tokenNumber = book.CurrentTokenNumberInSentence();
          int word = book.CurrentTokenWord();
          int label = book.CurrentTokenLabel();

          if (m_typeOfDepLabels == 2) {
            // Update the feature matrix with the last dependency label
            UpdateFeatureLabelVector(lastLabel, m_state);
          }
          
          // Run one step of the RNN to predict word
          // from lastWord, lastLabel and the last hidden state
          ForwardPropagateOneStep(lastWord, word, m_state);
          
          // For perplexity, we do not count OOV words...
          if ((word >= 0) && (word != 1)) {
            // Compute the log-probability of the current word
            int outputNodeClass =
            m_vocabularyStorage[word].classIndex + GetVocabularySize();
            double condProbaClass =
            m_state.OutputLayer[outputNodeClass];
            double condProbaWordGivenClass =
            m_state.OutputLayer[word];
            double logProbabilityWord =
            log10(condProbaClass * condProbaWordGivenClass);
            
            // Did we see already that word token (at that position)
            // in the sentence?
            if (logProbSentence.find(tokenNumber) == logProbSentence.end()) {
              // No: store the log-likelihood of that word
              logProbSentence[tokenNumber] = logProbabilityWord;
              // Contribute the log-likelihood to the sentence and corpus
              testLogProbability += logProbabilityWord;
              sentenceLogProbability += logProbabilityWord;
              uniqueWordCounter++;
              
              // Verbose
              if (m_debugMode) {
                cout << tokenNumber << "\t"
                << word << "\t"
                << logProbabilityWord << "\t"
                << m_vocabularyStorage[word].word << endl;
              }
            } else {
              // Safety check
              if (logProbSentence[tokenNumber] != logProbabilityWord) {
                cout << "logProbSentence[tokenNumber] = "
                     << logProbSentence[tokenNumber] << endl;
                cout << "logProbabilityWord = "
                     << logProbabilityWord << endl;
              }
            }
          } else {
            if (m_debugMode) {
              // Out-of-vocabulary words have probability 0 and index -1
              cout << "-1\t0\tOOV\n";
            }
            numUnk++;
          }
          
          // Store the current state s(t) at the end of the input layer vector
          // so that it can be used as s(t-1) at the next step
          ForwardPropagateRecurrentConnectionOnly(m_state);
          
          // Rotate the word history by one
          ForwardPropagateWordHistory(m_state, lastWord, word);
          // Update the last label
          lastLabel = label;
          
          // Go to the next word
          ok = (book.NextTokenInUnroll() >= 0);
        } // Loop over tokens in the unroll of a sentence
        book.NextUnrollInSentence();
      } // Loop over unrolls of a sentence
      
      // Reset the table of word token probabilities
      logProbSentence.clear();
      // Store the log-probability of the sentence
      sentenceScores.push_back(sentenceLogProbability);
      
      book.NextSentence();
    } // Loop over sentences
  } // Loop over books
  
  // Return the total logProbability
  cout << "Log probability: " << testLogProbability
  << ", number of words " << uniqueWordCounter
  << " (" << numUnk << " <unk>,"
  << " " << sentenceScores.size() << " sentences)\n";
  double perplexity = 0;
  if (uniqueWordCounter > 0) {
    perplexity = ExponentiateBase10(-testLogProbability / (double)uniqueWordCounter);
  }
  cout << "PPL net (perplexity without OOV): " << perplexity << endl;
  return testLogProbability;
}
