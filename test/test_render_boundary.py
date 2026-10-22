#!/usr/bin/env python3
"""Keep native solid-shape construction behind the logical rendering boundary."""

from pathlib import Path
import re
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TOKENS = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL)
NATIVE_HEADER = re.compile(r'^\s*#\s*include\s*[<"][^>"\n]*(?:Rect|Border)PassElement\.hpp[>"]', re.MULTILINE)
NATIVE_TYPE = re.compile(r'\bC(?:Rect|Border)PassElement\b')
DIRECT_DRAW = re.compile(r'\brender(?:Rect|Border)\s*\(')
PHYSICAL_SHAPE = re.compile(r'\b(?:physicalRenderBox|physicalBox|physicalShape|physicalRadius|logicalBorderWidth)\s*\(')


def violations(relative: Path, source: str) -> list[str]:
    # Preserve offsets for useful file/line diagnostics while ignoring prose.
    code = TOKENS.sub(lambda match: re.sub(r'[^\n]', ' ', match.group()), source)
    rules = []
    if relative != Path('PassElements.cpp'):
        rules += [(NATIVE_HEADER, source, 'native shape include'), (NATIVE_TYPE, code, 'native shape type'), (DIRECT_DRAW, code, 'direct native shape draw')]
    if relative not in {Path('PassElements.cpp'), Path('PassElements.hpp'), Path('RenderGeometry.hpp')}:
        rules.append((PHYSICAL_SHAPE, code, 'physical shape conversion'))
    result = []
    for pattern, text, reason in rules:
        for match in pattern.finditer(text):
            line = text.count('\n', 0, match.start()) + 1
            result.append(f'{relative}:{line}: {reason}; use renderPass logical factories')
    return result


def scan(root: Path) -> list[str]:
    result = []
    for path in sorted(root.rglob('*')):
        if path.suffix in {'.cpp', '.hpp'}:
            result.extend(violations(path.relative_to(root), path.read_text()))
    return result


class RenderBoundaryTests(unittest.TestCase):
    def test_production_source_uses_only_shared_native_shape_boundary(self):
        self.assertEqual(scan(ROOT / 'src'), [])

    def test_constructor_includes_and_aliases_are_rejected(self):
        samples = [
            '#include <hyprland/src/render/pass/RectPassElement.hpp>',
            '#include "hyprland/src/render/pass/BorderPassElement.hpp"',
            'out.emplace_back(makeUnique<CRectPassElement>(data));',
            'using NativeBorder = ::CBorderPassElement;',
        ]
        for sample in samples:
            with self.subTest(source=sample):
                self.assertTrue(violations(Path('Feature.cpp'), sample))

    def test_physical_conversion_and_direct_draw_bypasses_are_rejected(self):
        for sample in ['physicalRenderBox(box, scale);', 'renderGeometry::physicalShape(box, scale);',
                       'renderGeometry::physicalRadius(radius, scale);', 'physicalRadius(radius, scale);', 'renderGeometry::logicalBorderWidth(width);',
                       'gl->renderRect(box, color);', 'Render::renderBorder(box, color);']:
            with self.subTest(source=sample):
                self.assertTrue(violations(Path('Feature.cpp'), sample))

    def test_adapter_is_the_only_native_shape_owner(self):
        native = '#include <hyprland/src/render/pass/RectPassElement.hpp>\nauto p = makeUnique<CRectPassElement>(data);'
        self.assertEqual(violations(Path('PassElements.cpp'), native), [])
        self.assertTrue(violations(Path('nested/PassElements.cpp'), native))
        self.assertTrue(violations(Path('PassElements.hpp'), native))

    def test_fractional_texture_geometry_and_documentation_remain_allowed(self):
        source = '''// CRectPassElement and physicalRenderBox are private to the adapter.
        const char* label = "CBorderPassElement";
        CTexPassElement::SRenderData{.box = CBox{x * scale, y * scale, w * scale, h * scale}};
        renderPass::appendSolidFill(out, logical, color, scale);
        '''
        self.assertEqual(violations(Path('Feature.cpp'), source), [])

    def test_new_nested_features_are_also_checked(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            nested = root / 'new'
            nested.mkdir()
            (nested / 'Feature.cpp').write_text('auto shape = makeUnique<CBorderPassElement>(data);')
            self.assertEqual(len(scan(root)), 1)


if __name__ == '__main__':
    unittest.main()
