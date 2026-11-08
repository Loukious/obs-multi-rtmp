#include "pch.h"
#include "output-config.h"
bool EditLayoutEncoder(QWidget* parent, const std::string& id, nlohmann::json& params, int& bitrate, bool audio);

class EditOutputWidget : virtual public QDialog {
public:
    virtual ~EditOutputWidget() {}
};

EditOutputWidget* createEditOutputWidget(const std::string& targetid, QWidget* parent = 0);
