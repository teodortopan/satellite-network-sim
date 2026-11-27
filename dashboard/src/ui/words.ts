// Plain words for the project's names that several stories show: the laser layouts (patterns)
// and the groups of satellites (shells).

/** A layout's short name for a button or a chart line. */
export const PATTERN_SHORT: Record<string, string> = {
  three: '3 lasers each',
  'plus-grid': '4 lasers each',
  six: '6 lasers each',
};

/** What a layout means, in one clause. */
export const PATTERN_WORDS: Record<string, string> = {
  three: 'each satellite has three laser partners: the one ahead, the one behind, and one beside it',
  'plus-grid': 'each satellite has four laser partners: ahead, behind, left and right (a grid)',
  six: 'each satellite has six laser partners: ahead, behind, left, right, and one two rows away on each side',
};

export function patternShort(pattern: string): string {
  return PATTERN_SHORT[pattern] ?? pattern;
}

/** "each satellite has … (the layout “three”)", or the bare name for a layout without words. */
export function patternWords(pattern: string): string {
  const words = PATTERN_WORDS[pattern];
  return words === undefined ? `the laser layout is “${pattern}”` : `${words} (the layout called “${pattern}”)`;
}

/** A shell's name in plain words: "53deg" is the group tilted 53° to the equator. */
export function shellName(shell: string): string {
  return /^\d+(\.\d+)?deg$/.test(shell) ? `Tilted ${shell.replace(/deg$/, '°')}` : `Group ${shell}`;
}
