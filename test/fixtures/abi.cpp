// A standalone ELF fixture: it never loads into a compositor.
#ifndef TEST_ABI
#define TEST_ABI "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa_aq_0.14.0_hu_0.14.1_hg_0.5.1_hc_0.1.13_hlg_0.6.8"
#endif
#ifndef TEST_REVISION
#define TEST_REVISION 1
#endif

[[gnu::used, gnu::section(".hyprspace.abi")]]
static const char ABI[] = TEST_ABI;

namespace Render {
    class IHyprRenderer {
      public:
        void requiredSymbol();
    };
} // namespace Render

extern "C" int fixture(Render::IHyprRenderer* renderer) {
    renderer->requiredSymbol();
    return TEST_REVISION;
}
