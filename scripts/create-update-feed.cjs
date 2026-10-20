const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { buildVersion } = require('./release-version.cjs');

/** 从实际分发文件生成静态更新清单，客户端无需占用匿名 GitHub REST API 额度。 */
function createUpdateFeed(directory, version, repository = 'MikeQu223/Rhine-Music-Database') {
    const tag = 'v' + buildVersion(version).version;
    if (!/^[A-Za-z0-9_.-]+\/[A-Za-z0-9_.-]+$/.test(repository)) {
        throw new Error('Invalid release repository');
    }
    const base = `https://github.com/${repository}/releases`;
    const platforms = ['macOS-arm64.zip', 'Windows-x64.zip', 'Linux-x86_64.AppImage'];
    const assets = platforms.map((platform) => {
        const name = `RhineMusicDatabase-${tag}-${platform}`;
        const file = fs.readFileSync(path.join(directory, name));
        if (!file.length || file.length > 2 * 1024 * 1024 * 1024) {
            throw new Error(`Invalid release package size: ${name}`);
        }
        return {
            name,
            state: 'uploaded',
            size: file.length,
            digest: 'sha256:' + crypto.createHash('sha256').update(file).digest('hex'),
            browser_download_url: `${base}/download/${tag}/${name}`,
        };
    });
    const feed = {
        tag_name: tag,
        html_url: `${base}/tag/${tag}`,
        draft: false,
        prerelease: false,
        assets,
    };
    fs.writeFileSync(path.join(directory, 'update.json'), JSON.stringify(feed, null, 4) + '\n');
    return feed;
}
if (require.main === module) {
    createUpdateFeed(process.argv[2], process.argv[3], process.env.GITHUB_REPOSITORY);
}
module.exports = { createUpdateFeed };
