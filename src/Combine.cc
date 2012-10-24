/**************************************
  Simple multiChannel significance & limit calculator
***************************************/
#include "../interface/Combine.h"
#include <cstring>
#include <cerrno>
#include <iostream>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <unistd.h>
#include <errno.h>

#include <TCanvas.h>
#include <TFile.h>
#include <TGraphErrors.h>
#include <TIterator.h>
#include <TLine.h>
#include <TMath.h>
#include <TString.h>
#include <TSystem.h>
#include <TStopwatch.h>
#include <TTree.h>

#include <RooAbsData.h>
#include <RooAbsPdf.h>
#include <RooArgSet.h>
#include <RooCustomizer.h>
#include <RooDataHist.h>
#include <RooDataSet.h>
#include <RooFitResult.h>
#include <RooMsgService.h>
#include <RooPlot.h>
#include <RooRandom.h>
#include <RooRealVar.h>
#include <RooUniform.h>
#include <RooWorkspace.h>

#include <RooStats/HLFactory.h>
#include <RooStats/RooStatsUtils.h>
#include <RooStats/ModelConfig.h>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "../interface/LimitAlgo.h"
#include "../interface/utils.h"
#include "../interface/CloseCoutSentry.h"
#include "../interface/RooSimultaneousOpt.h"
#include "../interface/ToyMCSamplerOpt.h"
#include "../interface/AsimovUtils.h"

using namespace RooStats;
using namespace RooFit;
using namespace std;

LimitAlgo * algo, * hintAlgo;

Float_t t_cpu_, t_real_;
Float_t g_quantileExpected_ = -1.0;
TDirectory *outputFile = 0;
TDirectory *writeToysHere = 0;
TDirectory *readToysFromHere = 0;
int  verbose = 1;
bool withSystematics = 1;
bool doSignificance_ = 0;
bool lowerLimit_ = 0;
float cl = 0.95;
TTree *Combine::tree_ = 0;


Combine::Combine() :
    statOptions_("Common statistics options"),
    ioOptions_("Common input-output options"),
    miscOptions_("Common miscellaneous options"),
    rMin_(std::numeric_limits<float>::quiet_NaN()), 
    rMax_(std::numeric_limits<float>::quiet_NaN()) {
    namespace po = boost::program_options;
    statOptions_.add_options()
      ("systematics,S", po::value<bool>(&withSystematics)->default_value(true), "Add systematic uncertainties")
      ("cl,C",   po::value<float>(&cl)->default_value(0.95), "Confidence Level")
      ("rMin",   po::value<float>(&rMin_), "Override minimum value for signal strength")
      ("rMax",   po::value<float>(&rMax_), "Override maximum value for signal strength")
      ("prior",  po::value<std::string>(&prior_)->default_value("flat"), "Prior to use, for methods that require it and if it's not already in the input file: 'flat' (default), '1/sqrt(r)'")
      ("significance", "Compute significance instead of upper limit (works only for some methods)")
      ("lowerLimit",   "Compute the lower limit instead of the upper limit (works only for some methods)")
      ("hintStatOnly", "Ignore systematics when computing the hint")
      ("toysNoSystematics", "Generate all toys with the central value of the nuisance parameters, without fluctuating them")
      ("toysFrequentist",   "Generate all toys in the frequentist way. Note that this affects the toys generated by option '-t' that happen in the main loop, not the ones within the Hybrid(New) algorithm.")
      ("expectSignal", po::value<float>(&expectSignal_)->default_value(0.), "If set to non-zero, generate *signal* toys instead of background ones, with the specified signal strength.")
      ("expectSignalMass", po::value<float>(&expectSignalMass_)->default_value(-99.), "If set to non-zero, generate *signal* toys instead of background ones, with the specified mass.")            
      ("unbinned,U", "Generate unbinned datasets instead of binned ones (works only for extended pdfs)")
      ("generateBinnedWorkaround", "Make binned datasets generating unbinned ones and then binnning them. Workaround for a bug in RooFit.")
      ("setPhysicsModelParameters", po::value<string>(&setPhysicsModelParameterExpression_)->default_value(""), "Set the values of relevant physics model parameters. Give a comma separated list of parameter value assignments. Example: CV=1.0,CF=1.0")      
      ("setPhysicsModelParameterRanges", po::value<string>(&setPhysicsModelParameterRangeExpression_)->default_value(""), "Set the range of relevant physics model parameters. Give a colon separated list of parameter ranges. Example: CV=0.0,2.0:CF=0.0,5.0")      
      ;
    ioOptions_.add_options()
      ("saveWorkspace", "Save workspace to output root file")
      ("workspaceName,w", po::value<std::string>(&workspaceName_)->default_value("w"), "Workspace name, when reading it from or writing it to a rootfile.")
      ("modelConfigName",  po::value<std::string>(&modelConfigName_)->default_value("ModelConfig"), "ModelConfig name, when reading it from or writing it to a rootfile.")
      ("modelConfigNameB", po::value<std::string>(&modelConfigNameB_)->default_value("%s_bonly"), "Name of the ModelConfig for b-only hypothesis.\n"
                                                                                                  "If not present, it will be made from the singal model taking zero signal strength.\n"
                                                                                                  "A '%s' in the name will be replaced with the modelConfigName.")
      ("validateModel,V", "Perform some sanity checks on the model and abort if they fail.")
      ("saveToys",   "Save results of toy MC in output file")
      ;
    miscOptions_.add_options()
      ("newGenerator", po::value<bool>(&newGen_)->default_value(true), "Use new generator code for toys, fixes all issues with binned and mixed generation (equivalent of --newToyMC but affects the top-level toys from option '-t' instead of the ones within the HybridNew)")
      ("optimizeSimPdf", po::value<bool>(&optSimPdf_)->default_value(true), "Turn on special optimizations of RooSimultaneous. On by default, you can turn it off if it doesn't work for your workspace.")
      ("rebuildSimPdf", po::value<bool>(&rebuildSimPdf_)->default_value(false), "Rebuild simultaneous pdf from scratch to make sure constraints are correct (not needed in CMS workspaces)")
      ("compile", "Compile expressions instead of interpreting them")
      ("tempDir", po::value<bool>(&makeTempDir_)->default_value(false), "Run the program from a temporary directory (automatically on for text datacards or if 'compile' is activated)")
      ("guessGenMode", "Guess if to generate binned or unbinned based on dataset");
      ; 
}

void Combine::applyOptions(const boost::program_options::variables_map &vm) {
  if(withSystematics) {
    std::cout << ">>> including systematics" << std::endl;
  } else {
    std::cout << ">>> no systematics included" << std::endl;
  } 
  unbinned_ = vm.count("unbinned");
  generateBinnedWorkaround_ = vm.count("generateBinnedWorkaround");
  if (unbinned_ && generateBinnedWorkaround_) throw std::logic_error("You can't set generateBinnedWorkaround and unbinned options at the same time");
  guessGenMode_ = vm.count("guessGenMode");
  compiledExpr_ = vm.count("compile"); if (compiledExpr_) makeTempDir_ = true;
  doSignificance_ = vm.count("significance");
  lowerLimit_     = vm.count("lowerLimit");
  hintUsesStatOnly_ = vm.count("hintStatOnly");
  saveWorkspace_ = vm.count("saveWorkspace");
  toysNoSystematics_ = vm.count("toysNoSystematics");
  toysFrequentist_ = vm.count("toysFrequentist");
  if (toysNoSystematics_ && toysFrequentist_) throw std::logic_error("You can't set toysNoSystematics and toysFrequentist options at the same time");
  if (modelConfigNameB_.find("%s") != std::string::npos) {
      char modelBName[1024]; 
      sprintf(modelBName, modelConfigNameB_.c_str(), modelConfigName_.c_str());
      modelConfigNameB_ = modelBName;
  }
  mass_ = vm["mass"].as<float>();
  saveToys_ = vm.count("saveToys");
  validateModel_ = vm.count("validateModel");
}

bool Combine::mklimit(RooWorkspace *w, RooStats::ModelConfig *mc_s, RooStats::ModelConfig *mc_b, RooAbsData &data, double &limit, double &limitErr) {
  TStopwatch timer;
  bool ret = false;
  try {
    double hint = 0, hintErr = 0; bool hashint = false;
    if (hintAlgo) {
        if (hintUsesStatOnly_ && withSystematics) {
            withSystematics = false;
            hashint = hintAlgo->run(w, mc_s, mc_b, data, hint, hintErr, 0);
            withSystematics = true;
        } else {
            hashint = hintAlgo->run(w, mc_s, mc_b, data, hint, hintErr, 0);
        } 
    }
    limitErr = 0; // start with 0, as some algorithms don't compute it
    ret = algo->run(w, mc_s, mc_b, data, limit, limitErr, (hashint ? &hint : 0));    
  } catch (std::exception &ex) {
    std::cerr << "Caught exception " << ex.what() << std::endl;
    return false;
  }
  /* if ((ret == false) && (verbose > 3)) {
    std::cout << "Failed for method " << algo->name() << "\n";
    std::cout << "  --- DATA ---\n";
    utils::printRAD(&data);
    std::cout << "  --- MODEL ---\n";
    w->Print("V");
  } */
  timer.Stop(); t_cpu_ = timer.CpuTime()/60.; t_real_ = timer.RealTime()/60.;
  printf("Done in %.2f min (cpu), %.2f min (real)\n", t_cpu_, t_real_);
  return ret;
}

namespace { 
    struct ToCleanUp {
        TFile *tfile; std::string file, path;
        ToCleanUp() : tfile(0), file(""), path("") {}
        ~ToCleanUp() {
            if (tfile) { tfile->Close(); delete tfile; }
            if (!file.empty()) {  
                if (unlink(file.c_str()) == -1) std::cerr << "Failed to delete temporary file " << file << ": " << strerror(errno) << std::endl;
            }
            if (!path.empty()) {  boost::filesystem::remove_all(path); }
        }
    };
}
void Combine::run(TString hlfFile, const std::string &dataset, double &limit, double &limitErr, int &iToy, TTree *tree, int nToys) {
  ToCleanUp garbageCollect; // use this to close and delete temporary files

  TString tmpDir = "", tmpFile = "", pwd(gSystem->pwd());
  if (makeTempDir_) { 
      tmpDir = "roostats-XXXXXX"; tmpFile = "model";
      mkdtemp(const_cast<char *>(tmpDir.Data()));
      gSystem->cd(tmpDir.Data());
      garbageCollect.path = tmpDir.Data(); // request that we delete this dir when done
  } else if (!hlfFile.EndsWith(".hlf") && !hlfFile.EndsWith(".root")) {
      char buff[99]; snprintf(buff, 98, "roostats-XXXXXX");
      int fd = mkstemp(buff); close(fd);
      tmpFile = buff;
  }

  bool isTextDatacard = false, isBinary = false;
  TString fileToLoad = (hlfFile[0] == '/' ? hlfFile : pwd+"/"+hlfFile);
  if (!boost::filesystem::exists(fileToLoad.Data())) throw std::invalid_argument(("File "+fileToLoad+" does not exist").Data());
  if (hlfFile.EndsWith(".hlf") ) {
    // nothing to do
  } else if (hlfFile.EndsWith(".root")) {
    isBinary = true;
  } else {
    TString txtFile = fileToLoad.Data();
    TString options = TString::Format(" -m %f -D %s", mass_, dataset.c_str());
    if (!withSystematics) options += " --stat ";
    if (compiledExpr_)    options += " --compiled ";
    if (verbose > 1)      options += TString::Format(" --verbose %d", verbose-1);
    //-- Text mode: old default
    //int status = gSystem->Exec("text2workspace.py "+options+" '"+txtFile+"' -o "+tmpFile+".hlf"); 
    //isTextDatacard = true; fileToLoad = tmpFile+".hlf";
    //-- Binary mode: new default 
    int status = gSystem->Exec("text2workspace.py "+options+" '"+txtFile+"' -b -o "+tmpFile+".root"); 
    isBinary = true; fileToLoad = tmpFile+".root";
    if (status != 0 || !boost::filesystem::exists(fileToLoad.Data())) {
        throw std::invalid_argument("Failed to convert the input datacard from LandS to RooStats format. The lines above probably contain more information about the error.");
    }
    garbageCollect.file = fileToLoad;
  }

  if (getenv("CMSSW_BASE")) {
      gSystem->AddIncludePath(TString::Format(" -I%s/src ", getenv("CMSSW_BASE")));
      if (verbose > 3) std::cout << "Adding " << getenv("CMSSW_BASE") << "/src to include path" << std::endl;
  }
  if (getenv("ROOFITSYS")) {
      gSystem->AddIncludePath(TString::Format(" -I%s/include ", getenv("ROOFITSYS")));
      if (verbose > 3) std::cout << "Adding " << getenv("ROOFITSYS") << "/include to include path" << std::endl;
  }

  if (verbose <= 2) RooMsgService::instance().setGlobalKillBelow(RooFit::ERROR);
  // Load the model, but going in a temporary directory to avoid polluting the current one with garbage from 'cexpr'
  RooWorkspace *w = 0; RooStats::ModelConfig *mc = 0, *mc_bonly = 0;
  std::auto_ptr<RooStats::HLFactory> hlf(0);
  if (isBinary) {
    TFile *fIn = TFile::Open(fileToLoad); 
    garbageCollect.tfile = fIn; // request that we close this file when done

    w = dynamic_cast<RooWorkspace *>(fIn->Get(workspaceName_.c_str()));
    if (w == 0) {  
        std::cerr << "Could not find workspace '" << workspaceName_ << "' in file " << fileToLoad << std::endl; fIn->ls(); 
        throw std::invalid_argument("Missing Workspace"); 
    }
    if (verbose > 3) { std::cout << "Input workspace '" << workspaceName_ << "': \n"; w->Print("V"); }
    RooRealVar *MH = w->var("MH");
    if (MH!=0) {
      if (verbose > 2) std::cerr << "Setting variable 'MH' in workspace to the higgs mass " << mass_ << std::endl;
      MH->setVal(mass_);
    }
    mc       = dynamic_cast<RooStats::ModelConfig *>(w->genobj(modelConfigName_.c_str()));
    mc_bonly = dynamic_cast<RooStats::ModelConfig *>(w->genobj(modelConfigNameB_.c_str()));

    //*********************************************
    //set physics model parameters
    //*********************************************
    if (setPhysicsModelParameterExpression_ != "") {
      utils::setPhysicsModelParameters( setPhysicsModelParameterExpression_, mc);
      utils::setPhysicsModelParameters( setPhysicsModelParameterExpression_, mc_bonly);
    }
    if (setPhysicsModelParameterRangeExpression_ != "") {
      utils::setPhysicsModelParameterRanges( setPhysicsModelParameterRangeExpression_, mc);
      utils::setPhysicsModelParameterRanges( setPhysicsModelParameterRangeExpression_, mc_bonly);
    }

    if (mc == 0) {  
        std::cerr << "Could not find ModelConfig '" << modelConfigName_ << "' in workspace '" << workspaceName_ << "' in file " << fileToLoad << std::endl;
        throw std::invalid_argument("Missing ModelConfig"); 
    } else if (verbose > 3) { std::cout << "Workspace has a ModelConfig for signal called '" << modelConfigName_ << "', with contents:\n"; mc->Print("V"); }
    if (verbose > 3) { std::cout << "Input ModelConfig '" << modelConfigName_ << "': \n"; mc->Print("V"); }
    const RooArgSet *POI = mc->GetParametersOfInterest();
    if (POI == 0 || POI->getSize() == 0) throw std::invalid_argument("ModelConfig '"+modelConfigName_+"' does not define parameters of interest.");
    if (POI->getSize() > 1) std::cerr << "ModelConfig '" << modelConfigName_ << "' defines more than one parameter of interest. This is not supported in some statistical methods." << std::endl;
    if (mc->GetObservables() == 0) throw std::invalid_argument("ModelConfig '"+modelConfigName_+"' does not define observables.");
    if (mc->GetPdf() == 0) throw std::invalid_argument("ModelConfig '"+modelConfigName_+"' does not define a pdf.");
    if (rebuildSimPdf_ && typeid(*mc->GetPdf()) == typeid(RooSimultaneous)) {
        RooSimultaneous *newpdf = utils::rebuildSimPdf(*mc->GetObservables(), dynamic_cast<RooSimultaneous*>(mc->GetPdf()));
        w->import(*newpdf);
        mc->SetPdf(*newpdf);
    }
    if (optSimPdf_ && typeid(*mc->GetPdf()) == typeid(RooSimultaneous)) {
        RooSimultaneousOpt *optpdf = new RooSimultaneousOpt(static_cast<RooSimultaneous&>(*mc->GetPdf()), TString(mc->GetPdf()->GetName())+"_opt");
        w->import(*optpdf);
        mc->SetPdf(*optpdf);
    }
    if (mc_bonly == 0) {
        std::cerr << "Missing background ModelConfig '" << modelConfigNameB_ << "' in workspace '" << workspaceName_ << "' in file " << fileToLoad << std::endl;
        std::cerr << "Will make one from the signal ModelConfig '" << modelConfigName_ << "' setting signal strenth '" << POI->first()->GetName() << "' to zero"  << std::endl;
        w->factory("_zero_[0]");
        RooCustomizer make_model_s(*mc->GetPdf(),"_model_bonly_");
        make_model_s.replaceArg(*POI->first(), *w->var("_zero_"));
        RooAbsPdf *model_b = dynamic_cast<RooAbsPdf *>(make_model_s.build()); 
        model_b->SetName("_model_bonly_");
        w->import(*model_b);
        mc_bonly = new RooStats::ModelConfig(*mc);
        mc_bonly->SetPdf(*model_b);
    }
  } else {
    hlf.reset(new RooStats::HLFactory("factory", fileToLoad));
    w = hlf->GetWs();
    if (w == 0) {
        std::cerr << "Could not read HLF from file " <<  (hlfFile[0] == '/' ? hlfFile : pwd+"/"+hlfFile) << std::endl;
        return;
    }
    RooRealVar *MH = w->var("MH");
    if (MH==0) {
      std::cerr << "Could not find MH var in workspace '" << workspaceName_ << "' in file " << fileToLoad << std::endl;
      throw std::invalid_argument("Missing MH"); 
    }
    MH->setVal(mass_);
    if (w->set("observables") == 0) throw std::invalid_argument("The model must define a RooArgSet 'observables'");
    if (w->set("POI")         == 0) throw std::invalid_argument("The model must define a RooArgSet 'POI' for the parameters of interest");
    if (w->pdf("model_b")     == 0) throw std::invalid_argument("The model must define a RooAbsPdf 'model_b'");
    if (w->pdf("model_s")     == 0) throw std::invalid_argument("The model must define a RooAbsPdf 'model_s'");

    // create ModelConfig
    mc = new RooStats::ModelConfig(modelConfigName_.c_str(),"signal",w);
    mc->SetPdf(*w->pdf("model_s"));
    mc->SetObservables(*w->set("observables"));
    mc->SetParametersOfInterest(*w->set("POI"));
    if (w->set("nuisances"))         mc->SetNuisanceParameters(*w->set("nuisances"));
    if (w->set("globalObservables")) mc->SetGlobalObservables(*w->set("globalObservables"));
    if (w->pdf("prior")) mc->SetNuisanceParameters(*w->pdf("prior"));
    w->import(*mc, modelConfigName_.c_str());

    mc_bonly = new RooStats::ModelConfig(modelConfigNameB_.c_str(),"background",w);
    mc_bonly->SetPdf(*w->pdf("model_b"));
    mc_bonly->SetObservables(*w->set("observables"));
    mc_bonly->SetParametersOfInterest(*w->set("POI"));
    if (w->set("nuisances"))         mc_bonly->SetNuisanceParameters(*w->set("nuisances"));
    if (w->set("globalObservables")) mc_bonly->SetGlobalObservables(*w->set("globalObservables"));
    if (w->pdf("prior")) mc_bonly->SetNuisanceParameters(*w->pdf("prior"));
    w->import(*mc_bonly, modelConfigNameB_.c_str());
  }
  gSystem->cd(pwd);

  if (verbose <= 2) RooMsgService::instance().setGlobalKillBelow(RooFit::WARNING);

  const RooArgSet * observables = mc->GetObservables();     // not null
  const RooArgSet * POI = mc->GetParametersOfInterest();     // not null
  const RooArgSet * nuisances = mc->GetNuisanceParameters(); // note: may be null
  if (dynamic_cast<RooRealVar*>(POI->first()) == 0) throw std::invalid_argument("First parameter of interest is not a RooRealVar");

  if (w->data(dataset.c_str()) == 0) {
    if (isTextDatacard) { // that's ok: the observables are pre-set to the observed values
      RooDataSet *data_obs = new RooDataSet(dataset.c_str(), dataset.c_str(), *observables); 
      data_obs->add(*observables);
      w->import(*data_obs);
    } else {
      std::cout << "Dataset " << dataset.c_str() << " not found." << std::endl;
    }
  }

  if (verbose < -1) {
      RooMsgService::instance().setStreamStatus(0,kFALSE);
      RooMsgService::instance().setStreamStatus(1,kFALSE);
      RooMsgService::instance().setGlobalKillBelow(RooFit::FATAL);
  }


  if (!isnan(rMin_)) ((RooRealVar*)POI->first())->setMin(rMin_);
  if (!isnan(rMax_)) ((RooRealVar*)POI->first())->setMax(rMax_);

  if (mc->GetPriorPdf() == 0) {
      if (prior_ == "flat") {
          RooAbsPdf *prior = new RooUniform("prior","prior",*POI);
          w->import(*prior);
          mc->SetPriorPdf(*prior);
      } else if (prior_ == "1/sqrt(r)") {
          w->factory(TString::Format("EXPR::prior(\\\"1/sqrt(@0)\\\",%s)", POI->first()->GetName()));
          mc->SetPriorPdf(*w->pdf("prior"));
      } else if (!prior_.empty() && w->pdf(prior_.c_str()) != 0) {
          std::cout << "Will use prior '" << prior_ << "' in from the input workspace" << std::endl;
          mc->SetPriorPdf(*w->pdf(prior_.c_str()));
      } else {
          std::cerr << "Unknown prior '" << prior_ << "'. It's not 'flat' '1/sqrt(r)' or the name of a pdf in the model.\n" << std::endl;
          throw std::invalid_argument("Bad prior");
      }
  }

  if (withSystematics && nuisances == 0) {
      std::cout << "The signal model has no nuisance parameters. Please run the limit tool with no systematics (option -S 0)." << std::endl;
      std::cout << "To make things easier, I will assume you have done it." << std::endl;
      withSystematics = false;
  } else if (!withSystematics && nuisances != 0) {
    std::cout << "Will set nuisance parameters to constants: " ;
    utils::setAllConstant(*nuisances, true);
  }

  bool validModel = validateModel_ ? utils::checkModel(*mc, false) : true;
  if (validateModel_ && verbose) std::cout << "Sanity checks on the model: " << (validModel ? "OK" : "FAIL") << std::endl;

  // make sure these things are set consistently with what we expect
  if (mc->GetNuisanceParameters() && withSystematics) utils::setAllConstant(*mc->GetNuisanceParameters(), false);
  if (mc->GetGlobalObservables()) utils::setAllConstant(*mc->GetGlobalObservables(), true);
  if (mc_bonly->GetNuisanceParameters() && withSystematics) utils::setAllConstant(*mc_bonly->GetNuisanceParameters(), false);
  if (mc_bonly->GetGlobalObservables()) utils::setAllConstant(*mc_bonly->GetGlobalObservables(), true);


  w->saveSnapshot("clean", w->allVars());

  if (saveWorkspace_) {
    w->SetName(workspaceName_.c_str());
    outputFile->WriteTObject(w,workspaceName_.c_str());
  }

  tree_ = tree;

  bool isExtended = mc_bonly->GetPdf()->canBeExtended();
  RooRealVar *MH = w->var("MH");
  RooAbsData *dobs = w->data(dataset.c_str());
  RooAbsPdf  *genPdf = expectSignal_ > 0 ? mc->GetPdf() : mc_bonly->GetPdf();
  toymcoptutils::SimPdfGenInfo newToyMC(*genPdf, *observables, !unbinned_); RooRealVar *weightVar_ = 0;

  if (guessGenMode_ && genPdf->InheritsFrom("RooSimultaneous") && (dobs != 0)) {
      utils::guessChannelMode(dynamic_cast<RooSimultaneous&>(*mc->GetPdf()), *dobs, verbose);
      utils::guessChannelMode(dynamic_cast<RooSimultaneous&>(*mc_bonly->GetPdf()), *dobs, 0);
  }
  if (expectSignal_ > 0) { 
    if (POI->find("r")) {
      ((RooRealVar*)POI->find("r"))->setVal(expectSignal_);     
      if (MH && expectSignalMass_>0.) {
        MH->setVal(expectSignalMass_);        
      }
    }
  }

  if (nToys <= 0) { // observed or asimov
    iToy = nToys;
    if (iToy == -1) {	
     if (readToysFromHere != 0){
	dobs = dynamic_cast<RooAbsData *>(readToysFromHere->Get(TString::Format("toys/toy_asimov%g",expectSignal_)));
	if (dobs == 0) {
	  std::cerr << "Toy toy_asimov not found in " << readToysFromHere->GetName() << ". List follows:\n";
	  readToysFromHere->ls();
	  return;
	}
      }
      else{
        if (newGen_) {
            if (toysFrequentist_) {
                w->saveSnapshot("reallyClean", w->allVars());
                if (dobs == 0) throw std::invalid_argument("Frequentist Asimov datasets can't be generated without a real dataset to fit");
                RooArgSet gobsAsimov;
                dobs = asimovutils::asimovDatasetWithFit(mc, *dobs, gobsAsimov, expectSignal_, verbose);
                if (mc->GetGlobalObservables()) {
                    RooArgSet gobs(*mc->GetGlobalObservables());
                    gobs = gobsAsimov;
                    utils::setAllConstant(*mc->GetParametersOfInterest(), false);
                    w->saveSnapshot("clean", w->allVars());
                }
            } else {
                dobs = newToyMC.generateAsimov(weightVar_); // as simple as that
            }
        } else if (isExtended) {
            if (unbinned_) {
                throw std::invalid_argument("Asimov datasets can only be generated binned");
            } else {
                dobs = genPdf->generateBinned(*observables,RooFit::Extended(),RooFit::Asimov());
            }
	} else {
	  dobs = genPdf->generate(*observables,1,RooFit::Asimov());
	}
      }
    } else if (dobs == 0) {
      std::cerr << "No observed data '" << dataset << "' in the workspace. Cannot compute limit.\n" << std::endl;
      return;
    }
    if (saveToys_) {
	writeToysHere->WriteTObject(dobs, TString::Format("toy_asimov%g", expectSignal_));
    }
    std::cout << "Computing limit starting from " << (iToy == 0 ? "observation" : "expected outcome") << std::endl;
    if (MH) MH->setVal(mass_);    
    if (verbose > (isExtended ? 3 : 2)) utils::printRAD(dobs);
    if (mklimit(w,mc,mc_bonly,*dobs,limit,limitErr)) tree->Fill();
  }
  
  std::vector<double> limitHistory;
  std::auto_ptr<RooAbsPdf> nuisancePdf;
  if (nToys > 0) {
    double expLimit = 0;
    unsigned int nLimits = 0;
    w->loadSnapshot("clean");
    RooDataSet *systDs = 0;
    if (withSystematics && !toysNoSystematics_ && (readToysFromHere == 0)) {
      if (nuisances == 0) throw std::logic_error("Running with systematics enabled, but nuisances not defined.");
      nuisancePdf.reset(utils::makeNuisancePdf(expectSignal_ ? *mc : *mc_bonly));
      if (toysFrequentist_) {
          if (mc->GetGlobalObservables() == 0) throw std::logic_error("Cannot use toysFrequentist with no global observables");
          w->saveSnapshot("reallyClean", w->allVars());
          utils::setAllConstant(*mc->GetParametersOfInterest(), true); 
          {
              if (dobs == 0) throw std::logic_error("Cannot use toysFrequentist with no input dataset");
              CloseCoutSentry sentry(verbose < 3);
              genPdf->fitTo(*dobs, RooFit::Save(1), RooFit::Minimizer("Minuit2","minimize"), RooFit::Strategy(0), RooFit::Hesse(0), RooFit::Constrain(*(expectSignal_ ?mc:mc_bonly)->GetNuisanceParameters()));
          }
          utils::setAllConstant(*mc->GetParametersOfInterest(), false); 
          w->saveSnapshot("clean", w->allVars());
          systDs = nuisancePdf->generate(*mc->GetGlobalObservables(), nToys);
      } else {
          systDs = nuisancePdf->generate(*nuisances, nToys);
      } 
    }
    std::auto_ptr<RooArgSet> vars(genPdf->getVariables());
    algo->setNToys(nToys);

    for (iToy = 1; iToy <= nToys; ++iToy) {
      algo->setToyNumber(iToy-1);
      RooAbsData *absdata_toy = 0;
      if (readToysFromHere == 0) {
	w->loadSnapshot("clean");
	if (verbose > 3) utils::printPdf(genPdf);
	if (withSystematics && !toysNoSystematics_) {
	  *vars = *systDs->get(iToy-1);
          if (toysFrequentist_) w->saveSnapshot("clean", w->allVars());
	  if (verbose > 3) utils::printPdf(genPdf);
	}
        if (POI->find("r")) {
          if (expectSignal_) ((RooRealVar*)POI->find("r"))->setVal(expectSignal_);
        }
	std::cout << "Generate toy " << iToy << "/" << nToys << std::endl;
	if (isExtended) {
          if (newGen_) {
            absdata_toy = newToyMC.generate(weightVar_); // as simple as that
          } else if (unbinned_) {
    	      absdata_toy = genPdf->generate(*observables,RooFit::Extended());
          } else if (generateBinnedWorkaround_) {
              std::auto_ptr<RooDataSet> unbinn(genPdf->generate(*observables,RooFit::Extended()));
              absdata_toy = new RooDataHist("toy","binned toy", *observables, *unbinn);
          } else {
    	      absdata_toy = genPdf->generateBinned(*observables,RooFit::Extended());
          }
	} else {
	  RooDataSet *data_toy = genPdf->generate(*observables,1);
	  absdata_toy = data_toy;
	}
      } else {
	absdata_toy = dynamic_cast<RooAbsData *>(readToysFromHere->Get(TString::Format("toys/toy_%d",iToy)));
	if (absdata_toy == 0) {
	  std::cerr << "Toy toy_"<<iToy<<" not found in " << readToysFromHere->GetName() << ". List follows:\n";
	  readToysFromHere->ls();
	  return;
	}
      }
      if (verbose > (isExtended ? 3 : 2)) utils::printRAD(absdata_toy);
      w->loadSnapshot("clean");
      //if (verbose > 1) utils::printPdf(w, "model_b");
      if (mklimit(w,mc,mc_bonly,*absdata_toy,limit,limitErr)) {
	tree->Fill();
	++nLimits;
	expLimit += limit; 
        limitHistory.push_back(limit);
      }
      if (saveToys_) {
	writeToysHere->WriteTObject(absdata_toy, TString::Format("toy_%d", iToy));
      }
      delete absdata_toy;
    }
    if (weightVar_) delete weightVar_;
    expLimit /= nLimits;
    double rms = 0;
    for (std::vector<double>::const_iterator itl = limitHistory.begin(); itl != limitHistory.end(); ++itl) {
        rms += pow(*itl-expLimit, 2);
    }
    if (nLimits > 1) {
        rms = sqrt(rms/(nLimits-1)/nLimits);
        cout << "mean   expected limit: r < " << expLimit << " +/- " << rms << " @ " << cl*100 << "%CL (" <<nLimits << " toyMC)" << endl;
    } else {
        cout << "mean   expected limit: r < " << expLimit << " @ " << cl*100 << "%CL (" <<nLimits << " toyMC)" << endl;
    }
    sort(limitHistory.begin(), limitHistory.end());
    if (nLimits > 0) {
        double medianLimit = (nLimits % 2 == 0 ? 0.5*(limitHistory[nLimits/2-1]+limitHistory[nLimits/2]) : limitHistory[nLimits/2]);
        cout << "median expected limit: r < " << medianLimit << " @ " << cl*100 << "%CL (" <<nLimits << " toyMC)" << endl;
        double hi68 = limitHistory[min<int>(nLimits-1,  ceil(0.84  * nLimits))];
        double lo68 = limitHistory[min<int>(nLimits-1, floor(0.16  * nLimits))];
        double hi95 = limitHistory[min<int>(nLimits-1,  ceil(0.975 * nLimits))];
        double lo95 = limitHistory[min<int>(nLimits-1, floor(0.025 * nLimits))];
        cout << "   68% expected band : " << lo68 << " < r < " << hi68 << endl;
        cout << "   95% expected band : " << lo95 << " < r < " << hi95 << endl;
    }
  }

}

void Combine::commitPoint(bool expected, float quantile) {
    Float_t saveQuantile =  g_quantileExpected_;
    g_quantileExpected_ = quantile;
    tree_->Fill();
    g_quantileExpected_ = saveQuantile;
}

void Combine::addBranch(const char *name, void *address, const char *leaflist) {
    tree_->Branch(name,address,leaflist);
}
