#include <rack.hpp>
#include <osdialog.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>
#include "ScriptEngine.hpp"
#include <efsw/efsw.h>


using namespace rack;
Plugin* pluginInstance;


struct Prototype : Module {
	enum ParamIds {
		ENUMS(KNOB_PARAMS, NUM_ROWS),
		ENUMS(SWITCH_PARAMS, NUM_ROWS),
		NUM_PARAMS
	};
	enum InputIds {
		ENUMS(IN_INPUTS, NUM_ROWS),
		NUM_INPUTS
	};
	enum OutputIds {
		ENUMS(OUT_OUTPUTS, NUM_ROWS),
		NUM_OUTPUTS
	};
	enum LightIds {
		ENUMS(LIGHT_LIGHTS, NUM_ROWS * 3),
		ENUMS(SWITCH_LIGHTS, NUM_ROWS * 3),
		NUM_LIGHTS
	};

	std::string message;
	std::string path;
	std::string script;
	std::string engineName;
	std::mutex scriptMutex;
	ScriptEngine* scriptEngine = NULL;
	int frame = 0;
	int frameDivider;
	ProcessBlock* block;
	int bufferIndex = 0;

	efsw_watcher efsw = NULL;

	Prototype() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		for (int i = 0; i < NUM_ROWS; i++)
			configParam(KNOB_PARAMS + i, 0.f, 1.f, 0.5f, string::f("Knob %d", i + 1));
		for (int i = 0; i < NUM_ROWS; i++)
			configParam(SWITCH_PARAMS + i, 0.f, 1.f, 0.f, string::f("Switch %d", i + 1));

		block = new ProcessBlock;
		setPath("");
	}

	~Prototype() {
		setPath("");
		delete block;
	}

	void onReset() override {
		setScript(script);
	}

	void process(const ProcessArgs& args) override {
		// Frame divider for reducing sample rate
		if (++frame < frameDivider)
			return;
		frame = 0;

		// Inputs
		for (int i = 0; i < NUM_ROWS; i++)
			block->inputs[i][bufferIndex] = inputs[IN_INPUTS + i].getVoltage();

		// Process block
		if (++bufferIndex >= block->bufferSize) {
			bufferIndex = 0;

			// Block settings
			block->sampleRate = args.sampleRate;
			block->sampleTime = args.sampleTime;

			// Params
			for (int i = 0; i < NUM_ROWS; i++)
				block->knobs[i] = params[KNOB_PARAMS + i].getValue();
			for (int i = 0; i < NUM_ROWS; i++)
				block->switches[i] = params[SWITCH_PARAMS + i].getValue() > 0.f;
			float oldKnobs[NUM_ROWS];
			std::memcpy(oldKnobs, block->knobs, sizeof(block->knobs));

			// Run ScriptEngine's process function
			{
				std::lock_guard<std::mutex> lock(scriptMutex);
				// Check for certain inside the mutex
				if (scriptEngine) {
					if (scriptEngine->process()) {
						WARN("Script %s process() failed. Stopped script.", path.c_str());
						delete scriptEngine;
						scriptEngine = NULL;
						return;
					}
				}
			}

			// Params
			for (int i = 0; i < NUM_ROWS; i++) {
				if (block->knobs[i] != oldKnobs[i])
					params[KNOB_PARAMS + i].setValue(block->knobs[i]);
			}
			// Lights
			for (int i = 0; i < NUM_ROWS; i++)
				for (int c = 0; c < 3; c++)
					lights[LIGHT_LIGHTS + i * 3 + c].setBrightness(block->lights[i][c]);
			for (int i = 0; i < NUM_ROWS; i++)
				for (int c = 0; c < 3; c++)
					lights[SWITCH_LIGHTS + i * 3 + c].setBrightness(block->switchLights[i][c]);
		}

		// Outputs
		for (int i = 0; i < NUM_ROWS; i++)
			outputs[OUT_OUTPUTS + i].setVoltage(block->outputs[i][bufferIndex]);
	}

	void setPath(std::string path) {
		// Cleanup
		if (efsw) {
			efsw_release(efsw);
			efsw = NULL;
		}
		this->path = "";
		setScript("");

		if (path == "")
			return;

		this->path = path;
		loadPath();

		if (this->script == "")
			return;

		// Watch file
		std::string dir = string::directory(path);
		efsw = efsw_create(false);
		efsw_addwatch(efsw, dir.c_str(), watchCallback, false, this);
		efsw_watch(efsw);
	}

	void loadPath() {
		// Read file
		std::ifstream file;
		file.exceptions(std::ifstream::failbit | std::ifstream::badbit);
		try {
			file.open(path);
			std::stringstream buffer;
			buffer << file.rdbuf();
			std::string script = buffer.str();
			setScript(script);
		}
		catch (const std::runtime_error& err) {
			// Fail silently
		}
	}

	void setScript(std::string script) {
		std::lock_guard<std::mutex> lock(scriptMutex);
		// Reset script state
		if (scriptEngine) {
			delete scriptEngine;
			scriptEngine = NULL;
		}
		this->script = "";
		this->engineName = "";
		this->message = "";
		// Reset process state
		frameDivider = 32;
		frame = 0;
		*block = ProcessBlock();
		bufferIndex = 0;
		// Reset outputs and lights because they might hold old values
		for (int i = 0; i < NUM_ROWS; i++)
			outputs[OUT_OUTPUTS + i].setVoltage(0.f);
		for (int i = 0; i < NUM_ROWS; i++)
			for (int c = 0; c < 3; c++)
				lights[LIGHT_LIGHTS + i * 3 + c].setBrightness(0.f);
		for (int i = 0; i < NUM_ROWS; i++)
			for (int c = 0; c < 3; c++)
				lights[SWITCH_LIGHTS + i * 3 + c].setBrightness(0.f);

		if (script == "")
			return;
		this->script = script;

		// Create script engine from path extension
		std::string ext = string::filenameExtension(string::filename(path));
		scriptEngine = createScriptEngine(ext);
		if (!scriptEngine) {
			message = string::f("No engine for .%s extension", ext.c_str());
			return;
		}
		scriptEngine->module = this;

		// Run script
		if (scriptEngine->run(path, script)) {
			// Error message should have been set by ScriptEngine
			delete scriptEngine;
			scriptEngine = NULL;
			return;
		}
		this->engineName = scriptEngine->getEngineName();
	}

	static void watchCallback(efsw_watcher watcher, efsw_watchid watchid, const char* dir, const char* filename, enum efsw_action action, const char* old_filename, void* param) {
		Prototype* that = (Prototype*) param;
		if (action == EFSW_ADD || action == EFSW_DELETE || action == EFSW_MODIFIED || action == EFSW_MOVED) {
			// Check filename
			std::string pathFilename = string::filename(that->path);
			if (pathFilename == filename) {
				that->loadPath();
			}
		}
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();

		json_object_set_new(rootJ, "path", json_string(path.c_str()));
		json_object_set_new(rootJ, "script", json_stringn(script.data(), script.size()));

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		json_t* pathJ = json_object_get(rootJ, "path");
		if (pathJ) {
			std::string path = json_string_value(pathJ);
			setPath(path);
		}

		// Only get the script string if the script file wasn't found.
		if (this->path != "" && this->script == "") {
			WARN("Script file %s not found, using script in patch", this->path.c_str());
			json_t* scriptJ = json_object_get(rootJ, "script");
			if (scriptJ) {
				std::string script = std::string(json_string_value(scriptJ), json_string_length(scriptJ));
				setScript(script);
			}
		}
	}

	void loadScriptDialog() {
		std::string dir = asset::plugin(pluginInstance, "examples");
		char* pathC = osdialog_file(OSDIALOG_OPEN, dir.c_str(), NULL, NULL);
		if (!pathC) {
			return;
		}
		std::string path = pathC;
		std::free(pathC);

		setPath(path);
	}

	void reloadScript() {
		loadPath();
	}

	void saveScriptDialog() {
		if (script == "")
			return;

		std::string ext = string::filenameExtension(string::filename(path));
		std::string dir = asset::plugin(pluginInstance, "examples");
		std::string filename = "Untitled." + ext;
		char* newPathC = osdialog_file(OSDIALOG_SAVE, dir.c_str(), filename.c_str(), NULL);
		if (!newPathC) {
			return;
		}
		std::string newPath = newPathC;
		std::free(newPathC);
		// Add extension if user didn't specify one
		std::string newExt = string::filenameExtension(string::filename(newPath));
		if (newExt == "")
			newPath += "." + ext;

		std::ofstream f(newPath);
		f << script;
		// Set path directly
		path = newPath;
	}
};


void ScriptEngine::display(const std::string& message) {
	module->message = message;
}
void ScriptEngine::setFrameDivider(int frameDivider) {
	module->frameDivider = std::max(frameDivider, 1);
}
void ScriptEngine::setBufferSize(int bufferSize) {
	module->block->bufferSize = clamp(bufferSize, 1, MAX_BUFFER_SIZE);
}
ProcessBlock* ScriptEngine::getProcessBlock() {
	return module->block;
}


struct FileChoice : LedDisplayChoice {
	Prototype* module;

	void step() override {
		if (module && module->engineName != "")
			text = module->engineName;
		else
			text = "Script";
		text += ": ";
		if (module && module->path != "")
			text += string::filename(module->path);
		else
			text += "(click to load)";
	}

	void onAction(const event::Action& e) override {
		module->loadScriptDialog();
	}
};


struct MessageChoice : LedDisplayChoice {
	Prototype* module;

	void step() override {
		text = module ? module->message : "";
	}

	void draw(const DrawArgs& args) override {
		nvgScissor(args.vg, RECT_ARGS(args.clipBox));
		if (font->handle >= 0) {
			nvgFillColor(args.vg, color);
			nvgFontFaceId(args.vg, font->handle);
			nvgTextLetterSpacing(args.vg, 0.0);
			nvgTextLineHeight(args.vg, 1.08);

			nvgFontSize(args.vg, 12);
			nvgTextBox(args.vg, textOffset.x, textOffset.y, box.size.x - textOffset.x, text.c_str(), NULL);
		}
		nvgResetScissor(args.vg);
	}
};


struct PrototypeDisplay : LedDisplay {
	PrototypeDisplay() {
		box.size = mm2px(Vec(69.879, 27.335));
	}

	void setModule(Prototype* module) {
		FileChoice* fileChoice = new FileChoice;
		fileChoice->box.size.x = box.size.x;
		fileChoice->module = module;
		addChild(fileChoice);

		LedDisplaySeparator* fileSeparator = new LedDisplaySeparator;
		fileSeparator->box.size.x = box.size.x;
		fileSeparator->box.pos = fileChoice->box.getBottomLeft();
		addChild(fileSeparator);

		MessageChoice* messageChoice = new MessageChoice;
		messageChoice->box.pos = fileChoice->box.getBottomLeft();
		messageChoice->box.size.x = box.size.x;
		messageChoice->box.size.y = box.size.y - messageChoice->box.pos.y;
		messageChoice->module = module;
		addChild(messageChoice);
	}
};


struct LoadScriptItem : MenuItem {
	Prototype* module;
	void onAction(const event::Action& e) override {
		module->loadScriptDialog();
	}
};


struct ReloadScriptItem : MenuItem {
	Prototype* module;
	void onAction(const event::Action& e) override {
		module->reloadScript();
	}
};


struct SaveScriptItem : MenuItem {
	Prototype* module;
	void onAction(const event::Action& e) override {
		module->saveScriptDialog();
	}
};


struct PrototypeWidget : ModuleWidget {
	PrototypeWidget(Prototype* module) {
		setModule(module);
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Prototype.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(8.099, 64.401)), module, Prototype::KNOB_PARAMS + 0));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(20.099, 64.401)), module, Prototype::KNOB_PARAMS + 1));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(32.099, 64.401)), module, Prototype::KNOB_PARAMS + 2));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(44.099, 64.401)), module, Prototype::KNOB_PARAMS + 3));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(56.099, 64.401)), module, Prototype::KNOB_PARAMS + 4));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(68.099, 64.401)), module, Prototype::KNOB_PARAMS + 5));
		addParam(createParamCentered<PB61303>(mm2px(Vec(8.099, 80.151)), module, Prototype::SWITCH_PARAMS + 0));
		addParam(createParamCentered<PB61303>(mm2px(Vec(20.099, 80.151)), module, Prototype::SWITCH_PARAMS + 1));
		addParam(createParamCentered<PB61303>(mm2px(Vec(32.099, 80.151)), module, Prototype::SWITCH_PARAMS + 2));
		addParam(createParamCentered<PB61303>(mm2px(Vec(44.099, 80.151)), module, Prototype::SWITCH_PARAMS + 3));
		addParam(createParamCentered<PB61303>(mm2px(Vec(56.099, 80.151)), module, Prototype::SWITCH_PARAMS + 4));
		addParam(createParamCentered<PB61303>(mm2px(Vec(68.099, 80.151)), module, Prototype::SWITCH_PARAMS + 5));

		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.099, 96.025)), module, Prototype::IN_INPUTS + 0));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.099, 96.025)), module, Prototype::IN_INPUTS + 1));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(32.099, 96.025)), module, Prototype::IN_INPUTS + 2));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(44.099, 96.025)), module, Prototype::IN_INPUTS + 3));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(56.099, 96.025)), module, Prototype::IN_INPUTS + 4));
		addInput(createInputCentered<PJ301MPort>(mm2px(Vec(68.099, 96.025)), module, Prototype::IN_INPUTS + 5));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(8.099, 112.25)), module, Prototype::OUT_OUTPUTS + 0));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.099, 112.25)), module, Prototype::OUT_OUTPUTS + 1));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(32.099, 112.25)), module, Prototype::OUT_OUTPUTS + 2));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(44.099, 112.25)), module, Prototype::OUT_OUTPUTS + 3));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(56.099, 112.25)), module, Prototype::OUT_OUTPUTS + 4));
		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(68.099, 112.25)), module, Prototype::OUT_OUTPUTS + 5));

		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(8.099, 51.4)), module, Prototype::LIGHT_LIGHTS + 3 * 0));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(20.099, 51.4)), module, Prototype::LIGHT_LIGHTS + 3 * 1));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(32.099, 51.4)), module, Prototype::LIGHT_LIGHTS + 3 * 2));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(44.099, 51.4)), module, Prototype::LIGHT_LIGHTS + 3 * 3));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(56.099, 51.4)), module, Prototype::LIGHT_LIGHTS + 3 * 4));
		addChild(createLightCentered<MediumLight<RedGreenBlueLight>>(mm2px(Vec(68.099, 51.4)), module, Prototype::LIGHT_LIGHTS + 3 * 5));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(8.099, 80.151)), module, Prototype::SWITCH_LIGHTS + 0));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(20.099, 80.151)), module, Prototype::SWITCH_LIGHTS + 3 * 1));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(32.099, 80.151)), module, Prototype::SWITCH_LIGHTS + 3 * 2));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(44.099, 80.151)), module, Prototype::SWITCH_LIGHTS + 3 * 3));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(56.099, 80.151)), module, Prototype::SWITCH_LIGHTS + 3 * 4));
		addChild(createLightCentered<PB61303Light<RedGreenBlueLight>>(mm2px(Vec(68.099, 80.151)), module, Prototype::SWITCH_LIGHTS + 3 * 5));

		PrototypeDisplay* display = createWidget<PrototypeDisplay>(mm2px(Vec(3.16, 14.837)));
		display->setModule(module);
		addChild(display);
	}

	void appendContextMenu(Menu* menu) override {
		Prototype* module = dynamic_cast<Prototype*>(this->module);

		menu->addChild(new MenuEntry);

		LoadScriptItem* loadScriptItem = createMenuItem<LoadScriptItem>("Load script");
		loadScriptItem->module = module;
		menu->addChild(loadScriptItem);

		ReloadScriptItem* reloadScriptItem = createMenuItem<ReloadScriptItem>("Reload script");
		reloadScriptItem->module = module;
		menu->addChild(reloadScriptItem);

		SaveScriptItem* saveScriptItem = createMenuItem<SaveScriptItem>("Save script as");
		saveScriptItem->module = module;
		menu->addChild(saveScriptItem);
	}

	void onPathDrop(const event::PathDrop& e) override {
		Prototype* module = dynamic_cast<Prototype*>(this->module);
		if (module && e.paths.size() >= 1) {
			module->setPath(e.paths[0]);
		}
	}
};

void init(Plugin* p) {
	pluginInstance = p;

	p->addModel(createModel<Prototype, PrototypeWidget>("Prototype"));
}
