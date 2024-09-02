import XCTest
import SwiftTreeSitter
import TreeSitterScala

final class TreeSitterScalaTests: XCTestCase {
    func testCanLoadGrammar() throws {
        let parser = Parser()
        let language = Language(language: tree_sitter_scala())
        XCTAssertNoThrow(try parser.setLanguage(language),
                         "Error loading Scala grammar")
    }
}
