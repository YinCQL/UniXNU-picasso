"""Audited iPhone X userland inputs with the retained Darwin 23 kernel backend.

Hashes were recomputed from the local retail IPSW and the d22ap Erase identity.
Keep downloaded Apple data and owner private keys outside Git.
"""
PRODUCT = 'iPhone10,3'
DEVICE_CLASS = 'd22ap'
VERSION = '16.7.16'
BUILD = '20H392'
CHIP_ID = 0x8015
BOARD_ID = 0x06
FILENAME = 'iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw'
URL = ('https://updates.cdn-apple.com/2025FallFCS/fullrestores/122-77006/'
       '1158E502-D76D-4226-AA06-ED5E30CD1111/' + FILENAME)
IPSW_SHA256 = 'ece1194f7e233e9109c5790a8d23a4564a10a254e15ec5b09978a343265568fb'
KERNEL_SHA256 = '2ec297bac95bcba01046fd0148e97e7e5885c05a6c18827f39f534ba375c1221'
# KERNEL_SHA256 above identifies the retail iPhone kernel for input auditing.
# The port continues to build XNU 10063 and retains its qualified 21H461 kexts.
BACKEND_KERNEL_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
BACKEND_EARLY_SECURITY_INIT = 'com.apple.driver.AppleMobileFileIntegrity=0xfffffff005ab4ae0'
BACKEND_IPSW = dict(
    product='iPad7,1', device_class='j120ap', version='17.7.11', build='21H461',
    chip_id=0x8011, board_id=12,
    url='https://updates.cdn-apple.com/2025SpringFCS/fullrestores/122-77166/DCA6CFE8-90E9-4B92-880D-3DE171798E40/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw',
    kernel_member='kernelcache.release.ipad7',
    kernel_member_sha256='13b97fa2c5ce97f04408dcc28c41fb6b0d60b9440065197879acf44b2572bf7b',
)
OS_MEMBER = '087-86698-044.dmg'
OS_SHA256 = '80f426d5b8fca8552f95be427434f1911dbb4c2c6d28aad94f2426ddd0c92483'
LAUNCHD_SHA256 = '12370e5ee35bf28ae479b6c4b4455f4f4693ab803e37c5eb3032537c18cac61c'
OS_UUID = 'DADD93F8-DBBF-4B17-A1DF-0F11E64172F7'
SYSTEMOS_UUID = '96096B28-B6DC-4BC7-9D3A-2632A0FE53F1'
COMPONENTS = {
    'OS': (OS_MEMBER, OS_SHA256),
    'Cryptex1,SystemOS': ('087-86593-047.dmg', '9cd738f3b0169055549df84ad6d8d13e50a2da39af618ce0522ca4e06df86849'),
    'KernelCache': ('kernelcache.release.iphone10b', '2d6a13d040aaf87cec09bb6906d43e9bd8cea951e43d1aa1cd62707dfdc5aaec'),
    'DeviceTree': ('Firmware/all_flash/DeviceTree.d22ap.im4p', '480e8b0888949b4128b7aa2cd254497581c69e1bafa596a760b2fa955a8a6368'),
    'StaticTrustCache': ('Firmware/087-86698-044.dmg.trustcache', '63e01c03b31e1532a5581a24865490673f0b5c297b0199fe0095637c6100ef4b'),
    'Cryptex1,SystemTrustCache': ('Firmware/087-86593-047.dmg.trustcache', '9cc478fe3525e2683b5b9ea1eea0851eaebebdab21d206556233400e59d2e361'),
}
TRUST_MODULES = (
    ('087-86698-044.dmg.trustcache', 'b7ab28e0009900326ae685a88748008a9653e6bcbd70674135dac4eb5231ad64'),
    ('087-86593-047.dmg.trustcache', '8aae4c42fd70770d0d211c080982b3ef9ae5cf2cf98f918fd116fadb095b3f6a'),
)
TRUST_BUNDLE_SHA256 = 'eb1475f643f45a974ebd40596f6e2b1cdf40205d2ca3c6094d774886099399c3'
ROOT_IMAGE_SHA256 = '2c2aca3feb7585c91b80ec6b50cc1888ac9315f4c77d981566de5dd902eabefa'
ROOT_IMAGE_BYTES = 8380219392
ROOT_WINDOWS = (
    dict(name='OS', directory='os', volume_uuid=OS_UUID, offset=1048576,
         size=5635081728, sha256='6e87bfcaa8eca952e00b719f333d1f1060715a63566112267ad8730e1ed98906'),
    dict(name='SystemOS', directory='systemos', volume_uuid=SYSTEMOS_UUID,
         offset=5637144576, size=2743074816, sha256=COMPONENTS['Cryptex1,SystemOS'][1]),
)
OWNER_ANCHOR_SHA256 = '68c19aa4796e89fb48e0cc2f8243131fe23d31d3cf59554f7298a6cf8882e704'
POLICY_MANIFEST_SHA256 = {
    'OS': 'ea790d344b4823654ec32795790155d45d864ae2aa4a2a855c0b38b0e5d170f7',
    'SystemOS': '0933f17be6a4a7917c261fa9664ad97d976c7af4275ffad1fd0402e48330b174',
}

# These require userspace/device observations. They are not build-input errors
# and must not prevent SDK compilation and artifact checks from running.
RUNTIME_VALIDATION_PENDING = (
    'iOS 16 userspace contracts with the retained Darwin 23 / 21H461 kext backend need qualification, including MobileGraphics/IOSurface user-client dispatch',
    'AppleKeyStore state ABI, launchd override loading and logical display geometry need iOS 16 qualification',
)
