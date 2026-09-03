import sc.logger;

using Logger = sc::Logger<"Test">;
int main(int argc, char *argv[]) {
  Logger::info("String {}", 1);
  Logger::shutdown();
  return 0;
}
