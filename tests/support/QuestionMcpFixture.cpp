#include "QuestionMcpStdio.h"
#include "core/QuestionForm.h"
int main(int argc, char **argv) {
  return ade::runQuestionMcpStdio(
      argc, argv,
      ade::QuestionForm::toolDefinition()["function"]
          .toObject()["parameters"]
          .toObject());
}
